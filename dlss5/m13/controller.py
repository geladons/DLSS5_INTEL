# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.controller - the glue between the UI and the core modules. All side
# effects (processes, TCP, files, registry) go through here; widgets stay
# dumb. Every synchronous method returns (ok, message) so the UI can log it.
#
# RESPONSIVENESS RULE (owner-found 2026-09-23: "the app stops answering once
# DLSS starts"): the Tk main thread must NEVER touch a blocking probe. A busy
# m11d answers NRCT slowly, so a synchronous status() inside the 1 s poll
# froze the whole window. All probes (NRCT, process scans, dll hashing,
# registry) run in StateMonitor, a single background thread; the UI renders
# ctl.snapshot() (a plain dict, lock-copied, zero I/O). All mutating actions
# (start/stop/pause/gain) run on one ActionWorker thread; results come back
# through a callback the UI marshals via its own queue.
# ============================================================================
import os
import tempfile
import threading
import time

from . import config as config_mod
from . import deploy as deploy_mod
from . import gamelaunch
from . import paths
from .launcher import GameLauncher
from .worker import ActionWorker, WorkerSupervisor
from .daemonctl import M11dClient, DaemonError, DEFAULT_PORT
from .deploy import Deployer, LayerRegistry
from .phase30 import phase30_env_dict, write_phase30_knobs, PHASE30_DEFAULTS
from .processes import ManagedProcess, find_pid
from .screenmode import ScreenMode

M11D_EXE = paths.find("m11d_exe")
M11D_CWD = os.path.dirname(M11D_EXE)
M11D_LOG = os.path.join(paths.find("logs"), "m11d.log")

NRCT_TIMEOUT = 0.8          # busy daemon may answer slowly; never block the UI
MONITOR_PERIOD_S = 1.0


# ------------------------------------------------------------- monitoring --
class StateMonitor(threading.Thread):
    """Single background probe loop. The ONLY place blocking calls happen.
    Publishes an immutable dict snapshot; readers never do I/O."""

    def __init__(self, ctl, period=MONITOR_PERIOD_S):
        super().__init__(daemon=True, name="m13-monitor")
        self.ctl = ctl
        self.period = period
        self._stop_ev = threading.Event()
        self._lock = threading.Lock()
        self._snap = {}
        self._prev_running_games = set()
        self._x86_seen = False

    def stop(self):
        self._stop_ev.set()

    def snapshot(self):
        with self._lock:
            return dict(self._snap)

    def run(self):
        while not self._stop_ev.is_set():
            try:
                snap = self._collect()
            except Exception as e:          # a probe bug must not kill the loop
                snap = {"monitor_error": str(e)}
            with self._lock:
                self._snap = snap
            self._stop_ev.wait(self.period)

    def _collect(self):
        ctl = self.ctl
        snap = {}
        # daemon
        pid = ctl.daemon.pid
        snap["daemon_pid"] = pid
        snap["daemon_gain"] = None
        snap["daemon_frames"] = None
        snap["daemon_err"] = None
        if pid is not None:
            try:
                g, f = M11dClient(timeout=NRCT_TIMEOUT).status()
                snap["daemon_gain"], snap["daemon_frames"] = g, f
            except DaemonError as e:
                snap["daemon_err"] = str(e)
        # layers
        ls = ctl.layers_status()
        snap["layers_x64"] = ls["x64"]
        snap["layers_x86"] = ls["x86"]
        # screen mode
        snap["screen_pid"] = ctl.screen.proc.pid
        # games from cfg: {"exe": {"mode","arch","name"}}
        games = {}
        for exe, meta in (ctl.cfg.get("games") or {}).items():
            g = {"mode": meta.get("mode"), "arch": meta.get("arch"),
                 "name": meta.get("name") or
                 os.path.splitext(os.path.basename(exe))[0]}
            g["running"] = find_pid(os.path.basename(exe)) is not None
            g["paused"] = gamelaunch.mode_paused(g["mode"])
            g["dll"] = None
            g["guards"] = []
            if os.path.isdir(os.path.dirname(exe)):
                try:
                    g["dll"] = Deployer(os.path.dirname(exe)).mode_status(
                        g["mode"], g["arch"])
                    if g["mode"] == "dx9":
                        g["guards"] = deploy_mod.dx9_guard(
                            os.path.dirname(exe))
                except Exception as e:
                    g["guards"] = ["probe failed: %s" % e]
            games[exe] = g
        snap["games"] = games
        snap["weights_ok"] = ctl.weights_ok()
        snap["engine_ok"] = True
        snap["scale"] = ctl.cfg.get("scale")
        snap["frame_gen"] = ctl.cfg.get("frame_gen")
        snap["sharpness"] = ctl.cfg.get("sharpness")
        active_game = self._active_game(snap)
        snap["active_game"] = active_game
        snap["active_mode"] = self._active_mode(snap)
        p30 = ctl.cfg.get_phase30(active_game)
        snap["phase30"] = p30
        snap["intensity"] = p30["intensity"]
        snap["local_tone"] = p30["local_tone"]
        snap["local_structure"] = p30["local_structure"]
        snap["skin_structure"] = p30["skin_structure"]
        snap["auto_mask"] = p30["auto_mask"]
        snap["style_index"] = p30["style_index"]
        snap["color_strength"] = p30["color_strength"]

        currently_running = {exe for exe, g in games.items() if g.get("running")}
        has_running_x86 = any(
            g.get("arch") == "x86"
            for exe, g in games.items()
            if g.get("running")
        ) or (find_pid("gtaiv.exe") is not None) or (find_pid("GTAIV.exe") is not None)

        if has_running_x86:
            self._x86_empty_ticks = 0
            self._x86_seen = True
            # Auto-recover: an x86 game is alive but no daemon answers -
            # without a restart the layer would fall back to raw output for
            # the rest of the session (daemon crash, manual kill, reaper
            # racing a slow spawn). Bounded: one attempt per 30 ticks, max
            # 5 per monitor lifetime, so a permanently broken daemon does
            # not spin a restart loop.
            if (not ctl.daemon.running and self._x86_seen
                    and not ctl.client.alive()):
                last = getattr(self, "_daemon_restart_at", 0.0)
                tries = getattr(self, "_daemon_restarts", 0)
                now = time.monotonic()
                if tries < 5 and now - last >= 30.0:
                    self._daemon_restart_at = now
                    self._daemon_restarts = tries + 1
                    msg = "[controller] x86 game alive but daemon dead - restarting (try %d)" % (tries + 1)
                    print(msg, flush=True)
                    ok, m2 = ctl.daemon_start()
                    print("[controller] daemon restart: %s %s" % (ok, m2), flush=True)
        elif ctl.daemon.running:
            self._x86_empty_ticks = getattr(self, "_x86_empty_ticks", 0) + 1
        else:
            # No daemon to protect: stale tick debt from a daemonless gap must
            # not instantly reap a daemon that appears mid-launch, before the
            # game's process shows up in the snapshot.
            self._x86_empty_ticks = 0
            self._x86_seen = False

        exited_games = self._prev_running_games - currently_running
        if exited_games and not currently_running:
            gamelaunch.reset_runtime_flags()
            ctl._active_game_exe = None
        if exited_games or not has_running_x86:
            if (not has_running_x86 and self._x86_seen and ctl.daemon.running
                    and self._x86_empty_ticks >= 10):
                msg = "[controller] game exited; stopping x86 background daemon"
                print(msg, flush=True)
                if ctl.worker and getattr(ctl.worker, "on_result", None):
                    try:
                        ctl.worker.on_result(True, msg)
                    except Exception:
                        pass
                ctl.daemon.stop()
                snap["daemon_pid"] = None
                snap["daemon_gain"] = None
                snap["daemon_frames"] = None
                snap["daemon_err"] = None
                self._x86_seen = False
        self._prev_running_games = currently_running

        return snap

    @staticmethod
    def _active_game(snap):
        for exe, g in (snap.get("games") or {}).items():
            if g.get("running"):
                return exe
        return None

    @classmethod
    def _active_mode(cls, snap):
        """Which processing path is live right now (pause/gain target)."""
        exe = cls._active_game(snap)
        if exe:
            return snap["games"][exe]["mode"]      # dx9 | dx11 | dx12
        if snap.get("screen_pid") is not None:
            return "screen"
        return None


# -------------------------------------------------------------- controller --
class M13Controller:
    def __init__(self, cfg=None):
        self.cfg = cfg or config_mod.Config()
        self._migrate_legacy_games()
        self._remove_legacy_hudmask_file()
        os.makedirs(os.path.dirname(M11D_LOG), exist_ok=True)
        self.daemon = ManagedProcess("m11d.exe", M11D_EXE, M11D_CWD, M11D_LOG)
        self.screen = ScreenMode()
        self.client = M11dClient(timeout=NRCT_TIMEOUT)
        self.monitor = StateMonitor(self)
        self.worker = None          # created by the UI (needs its callback)
        self.supervisor = None      # WorkerSupervisor, starts with the worker
        self.launcher = GameLauncher(self, log_fn=self._worker_log)
        self.launched_exes = set()  # launch targets started BY US this
                                    # session (they carry the layer freeze env)
        self._active_game_exe = None

        # Initialize runtime environment variables from config
        scale = float(self.cfg.get("scale") or 0.50)
        s_val = str(scale)
        os.environ["DLSS5_SCALE"] = s_val
        os.environ["NR_LAYER_SCALE"] = s_val
        os.environ["M12_SCALE"] = s_val
        os.environ["DLSS5_FRAMEGEN"] = "1" if self.cfg.get("frame_gen") else "0"
        os.environ["DLSS5_FG_MODE"] = "2" if self.cfg.get("frame_gen") else "1"
        os.environ["DLSS5_SHARPNESS"] = str(float(self.cfg.get("sharpness") or 0.8))
        p30 = self.cfg.get_phase30()
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        # Seed weights/shader dirs for the in-process d5c_chain: launched
        # games get the per-mode values via env_extra in launch_game; the
        # process env covers any consumer inheriting our environment.
        os.environ.update(gamelaunch.engine_env(self.cfg))

        self.monitor.start()

    def _migrate_legacy_games(self):
        """v1 config had single dx9_game/dx12_game keys -> games dict.
        v3 -> v4: entries pointing at launcher STUBS (the owner added
        Launcher.exe / the GOG stub as the game) are retargeted to the real
        renderer binary; the stub becomes launch_exe so the crack chain
        still runs. Dead paths are dropped."""
        games = dict(self.cfg.get("games") or {})
        changed = False
        # games keys are normpath'd on load (config._dedupe_games) - the
        # legacy keys still hold raw strings, so compare normalized or the
        # same game would be re-added as a slash-duplicate every start.
        norm_keys = {os.path.normcase(os.path.normpath(k)) for k in games}
        for key, mode in (("dx9_game", "dx9"), ("dx12_game", "dx12")):
            exe = self.cfg.get(key)
            if exe and os.path.normcase(os.path.normpath(exe)) \
                    not in norm_keys:
                games[os.path.normpath(exe)] = {
                    "mode": mode,
                    "arch": "x86" if mode == "dx9" else "x64",
                    "name": os.path.splitext(
                        os.path.basename(exe))[0]}
                norm_keys.add(os.path.normcase(os.path.normpath(exe)))
                changed = True
        from . import gamescan
        for exe in list(games):
            if not os.path.exists(exe):
                del games[exe]
                changed = True
                continue
            low = os.path.basename(exe).lower()
            _arch, apis = gamescan.pe_info(exe)
            stub = (not apis) or any(w in low for w in gamescan.MAIN_BAD)
            if not stub:
                continue
            g = gamescan._game_from_group(os.path.dirname(exe), "saved")
            if not g or g.exe.lower() == exe.lower():
                continue
            meta = games.pop(exe)
            if g.exe in games:            # real entry exists: just teach it
                games[g.exe]["launch_exe"] = exe      # the wrapper
            else:
                meta["launch_exe"] = exe
                meta["mode"] = g.mode or meta.get("mode")
                meta["arch"] = g.arch or meta.get("arch")
                games[g.exe] = meta
            changed = True
        for exe in games:
            if "hudmask" in games[exe]:
                del games[exe]["hudmask"]
                changed = True
        if changed:
            self.cfg.set("games", games)

    def _remove_legacy_hudmask_file(self):
        """Stage-24 positional HUD mask is gone; drop its leftover config."""
        try:
            os.remove(os.path.join(tempfile.gettempdir(), "m13_hudmask.cfg"))
        except OSError:
            pass

    # ------------------------------------------------------- auto setup ----
    def autosetup(self):
        """Hands-free bring-up (runs on the worker at startup): find weights,
        register layers, start the daemon. Every step is idempotent and
        logged; failures surface as messages, never exceptions."""
        steps = []
        if not self.weights_ok():
            found = self._autodetect_weights()
            if found:
                self.cfg.set("weights_path", found)
                steps.append("weights auto-found: %s" % found)
            else:
                steps.append("WEIGHTS MISSING - point at "
                             "dlssnr-logical.safetensors (Settings)")
        ls = self.layers_status()
        if not all(ls.values()):
            ok, msg = self.layers_register()
            steps.append("auto: %s" % msg)
        else:
            steps.append("layers already registered")
        steps.append("in-process DLSS 5 ready")
        return True, " | ".join(steps)

    def _autodetect_weights(self):
        """*.safetensors in the bundle weights\\ dir (or dev work\\mlxw)."""
        d = paths.find("weights")
        try:
            cands = [os.path.join(d, n) for n in os.listdir(d)
                     if n.endswith(".safetensors")]
        except OSError:
            return None
        return cands[0] if cands else None

    # ----------------------------------------------------------- lifetime --
    def start_worker(self, on_result):
        self.worker = ActionWorker(on_result)
        self.worker.start()
        self.supervisor = WorkerSupervisor(self)
        self.supervisor.start()

    def shutdown(self):
        self.monitor.stop()
        if self.worker:
            self.worker.stop()
        if self.supervisor:
            self.supervisor.stop()
        self._daemon_shutdown()

    def _daemon_shutdown(self):
        """Reap the x86 daemon on manager exit. It is only kept when an x86
        game is still running: killing it mid-game freezes that game's
        picture on the held frame, and the monitor that normally reaps it
        stops with the manager, so without this the daemon (VRAM arena +
        ~500 MB) leaks until reboot."""
        if not self.daemon.running:
            return
        for exe, meta in (self.cfg.get("games") or {}).items():
            if meta.get("arch") == "x86" and find_pid(os.path.basename(exe)):
                print("[controller] x86 game still running; daemon left up",
                      flush=True)
                return
        self._ensure_daemon_stopped()

    def _worker_log(self, msg):
        """Forward a mid-action note to the UI log when a worker exists."""
        w = self.worker
        if w and getattr(w, "on_result", None):
            try:
                w.on_result(True, msg)
            except Exception:
                pass

    def snapshot(self):
        return self.monitor.snapshot()

    def submit(self, fn, *args):
        """Queue an action on the worker; result -> UI log via callback."""
        if self.worker is None:
            raise RuntimeError("worker not started")
        self.worker.submit(fn, args)

    # ------------------------------------------------------------- daemon --
    def weights_ok(self):
        p = self.cfg.get("weights_path")
        return bool(p) and os.path.exists(p)

    def daemon_status(self):
        """Synchronous probe (worker thread only - never the Tk thread)."""
        out = {"running": self.daemon.running, "gain": None, "frames": None,
               "error": None}
        if out["running"]:
            try:
                g, f = self.client.status()
                out["gain"], out["frames"] = g, f
            except DaemonError as e:
                out["error"] = str(e)
        return out

    def daemon_start(self):
        if not self.weights_ok():
            return False, "set the weights path first (Settings tab)"
        if self.daemon.running:
            return False, "m11d already running (pid %s)" % self.daemon.pid
        notes = []
        if self.screen.running:
            # VRAM EXCLUSIVITY: m11d and m8blive each reserve an ~11.7 GB
            # arena; the Arc Pro B50 has 16 GB - both at once = OOM crash
            # (that was "screen mode kills the daemon").
            ok, msg = self.screen.stop()
            notes.append("screen overlay stopped (%s)" % msg)
        p30 = self.cfg.get_phase30()
        args = ["--port", str(DEFAULT_PORT),
                "--gain", "%.3f" % float(self.cfg.get("gain") or 1.0),
                "--intensity", "%.3f" % p30["intensity"],
                "--local-tone", "%.3f" % p30["local_tone"],
                "--local-structure", "%.3f" % p30["local_structure"],
                "--skin-structure", "%.3f" % p30["skin_structure"],
                "--auto-mask", "1" if p30["auto_mask"] else "0",
                "--style-index", str(p30["style_index"]),
                "--color-strength", "%.3f" % p30["color_strength"],
                "--scale", "%.2f" % float(self.cfg.get("scale") or 0.50),
                "--framegen", "2" if self.cfg.get("frame_gen") else "0",
                "--weights", self.cfg.get("weights_path")]
        ok, msg = self.daemon.start(args)
        if notes:
            msg = " | ".join(notes + [msg])
        return ok, msg

    def daemon_stop(self):
        return self.daemon.stop()

    def _knob_blob(self, p30):
        """Phase30 dict plus the live engine knobs. The knob file is the
        only channel a RUNNING in-process game can see (its env froze at
        launch), so every write carries the current scale/gain/blend/
        sharpness/frame_gen next to the 7 vendor params."""
        blob = dict(p30)
        blob["gain"] = float(self.cfg.get("gain") or 1.0)
        blob["blend"] = float(self.cfg.get("blend") or 1.0)
        blob["scale"] = float(self.cfg.get("scale") or 0.50)
        blob["sharpness"] = float(self.cfg.get("sharpness") or 0.8)
        blob["frame_gen"] = 1 if self.cfg.get("frame_gen") else 0
        return blob

    # --------------------------------------------------------------- gain --
    def set_gain(self, gain):
        """Live gain push. Daemon up: NRCT, next processed frame; if a game
        frame is FROZEN (photo mode) the layer/proxy is asked to reprocess
        the held raw frame so the frozen picture updates. Screen mode up:
        the knob FILE retunes m8blive live (no restart - a restart costs
        the ~30 s weights upload and looks like a crash). Always persisted."""
        gain = max(0.0, min(16.0, float(gain)))
        self.cfg.set("gain", gain)
        g_val = str(gain)
        os.environ["DLSS5_GAIN"] = g_val
        os.environ["NR_LAYER_GAIN"] = g_val
        os.environ["M12_GAIN"] = g_val
        p30 = self.cfg.get_phase30()
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                g, _ = self.client.set_gain(gain)
            except DaemonError as e:
                return False, "gain saved but daemon push failed: %s" % e
            if gamelaunch.freeze_active():
                gamelaunch.reproc_bump()
                return True, ("gain -> %.3f (live; frozen frame "
                              "reprocessing)" % g)
            return True, "gain -> %.3f (live)" % g
        if self.screen.running:
            from .screenmode import write_knobs
            write_knobs(gain, float(self.cfg.get("blend")),
                        float(self.cfg.get("scale") or 0.50),
                        bool(self.cfg.get("frame_gen")),
                        phase30=self.cfg.get_phase30())
            return True, "gain -> %.3f (live, screen mode)" % gain
        if gamelaunch.freeze_active():
            # No daemon to race: the knob file/env are already written.
            # Covers the x64 in-process frozen frame (was never bumped).
            gamelaunch.reproc_bump()
            return True, ("gain -> %.3f (frozen frame reprocessing)"
                          % gain)
        return False, "daemon down - gain saved for next start"

    def set_blend(self, blend):
        """Live blend push (vendor mix factor 0..1). Same routing as gain."""
        blend = max(0.0, min(1.0, float(blend)))
        self.cfg.set("blend", blend)
        b_val = str(blend)
        os.environ["DLSS5_BLEND"] = b_val
        os.environ["NR_LAYER_BLEND"] = b_val
        os.environ["M12_BLEND"] = b_val
        # Knob file BEFORE any reproc bump: the in-process layer polls it as
        # the live channel, and a frozen-frame reprocess must see this value.
        write_phase30_knobs(self._knob_blob(self.cfg.get_phase30()))
        if self.daemon.running:
            try:
                self.client.set_blend(blend)
            except DaemonError as e:
                return False, "blend saved but daemon push failed: %s" % e
            if gamelaunch.freeze_active():
                # AFTER the NRCT push: a resend must see the new value.
                gamelaunch.reproc_bump()
                return True, ("blend -> %.3f (live; frozen frame "
                              "reprocessing)" % blend)
            return True, "blend -> %.3f (live)" % blend
        if self.screen.running:
            from .screenmode import write_knobs
            write_knobs(float(self.cfg.get("gain")), blend,
                        float(self.cfg.get("scale") or 0.50),
                        bool(self.cfg.get("frame_gen")),
                        phase30=self.cfg.get_phase30())
            return True, "blend -> %.3f (live, screen mode)" % blend
        if gamelaunch.freeze_active():
            gamelaunch.reproc_bump()
            return True, ("blend -> %.3f (live; frozen frame "
                          "reprocessing)" % blend)
        return True, "blend -> %.3f" % blend

    # ------------------------------------------------------------- scale --
    def set_scale(self, scale):
        """Render scale (0.33..1.0). Updates config, sets environment variables
        DLSS5_SCALE, NR_LAYER_SCALE, M12_SCALE, and bumps reproc if frozen."""
        scale = max(0.33, min(1.0, float(scale)))
        self.cfg.set("scale", scale)
        s_val = str(scale)
        os.environ["DLSS5_SCALE"] = s_val
        os.environ["NR_LAYER_SCALE"] = s_val
        os.environ["M12_SCALE"] = s_val
        # Knob file BEFORE any reproc bump (see set_blend); dlss5_set_scale
        # re-inits the engine on the next process_frame - that is intended.
        write_phase30_knobs(self._knob_blob(self.cfg.get_phase30()))
        if self.daemon.running:
            try:
                self.client.set_scale(scale)
            except DaemonError as e:
                return False, "scale saved but daemon push failed: %s" % e
            if gamelaunch.freeze_active():
                # AFTER the NRCT push: a resend must see the new value.
                gamelaunch.reproc_bump()
                return True, ("scale -> %.2f (live; frozen frame "
                              "reprocessing)" % scale)
            return True, "scale -> %.2f (live)" % scale
        if self.screen.running:
            from .screenmode import write_knobs
            write_knobs(float(self.cfg.get("gain")),
                        float(self.cfg.get("blend")),
                        scale,
                        bool(self.cfg.get("frame_gen")),
                        phase30=self.cfg.get_phase30())
            return True, "scale -> %.2f (live, screen mode)" % scale
        if gamelaunch.freeze_active():
            gamelaunch.reproc_bump()
            return True, ("scale -> %.2f (frozen frame reprocessing)"
                          % scale)
        return True, "scale -> %.2f" % scale

    def set_framegen(self, enable):
        """Frame Generation toggle. Updates config, sets DLSS5_FRAMEGEN=1/0."""
        enable = bool(enable)
        self.cfg.set("frame_gen", enable)
        os.environ["DLSS5_FRAMEGEN"] = "1" if enable else "0"
        # Knob file BEFORE any reproc bump (see set_blend).
        write_phase30_knobs(self._knob_blob(self.cfg.get_phase30()))
        if self.daemon.running:
            try:
                self.client.set_framegen(enable)
            except Exception:
                pass
        if self.screen.running:
            from .screenmode import write_knobs
            write_knobs(float(self.cfg.get("gain")),
                        float(self.cfg.get("blend")),
                        float(self.cfg.get("scale") or 0.50),
                        enable,
                        phase30=self.cfg.get_phase30())
            return True, "framegen -> %s (live, screen mode)" % ("ON" if enable else "OFF")
        if gamelaunch.freeze_active():
            gamelaunch.reproc_bump()
        return True, "framegen -> %s" % ("ON" if enable else "OFF")

    def set_sharpness(self, val):
        """FSR RCAS sharpness (0.0..1.0). Updates config, sets DLSS5_SHARPNESS."""
        val = max(0.0, min(1.0, float(val)))
        self.cfg.set("sharpness", val)
        os.environ["DLSS5_SHARPNESS"] = str(val)
        os.environ["NR_LAYER_SHARPNESS"] = str(val)
        # Knob file BEFORE any reproc bump (see set_blend).
        write_phase30_knobs(self._knob_blob(self.cfg.get_phase30()))
        if gamelaunch.freeze_active():
            gamelaunch.reproc_bump()
        return True, "sharpness -> %.2f" % val

    # ------------------------------------------------------------- phase30 --
    def set_intensity(self, intensity, exe=None):
        """Phase 30 compose mix (0.0..2.0, public baseline 1.0). Owns the
        headBlend compose slot - independent from the residual gain knob.
        Same routing as set_gain: config+env+knob file always, NRCT cmd 15
        when a daemon is up, frozen-frame reprocess afterwards."""
        intensity = max(0.0, min(2.0, float(intensity)))
        self.cfg.set("intensity", intensity)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_intensity(intensity)
            except DaemonError as e:
                return False, "intensity saved but daemon push failed: %s" % e
            if gamelaunch.freeze_active():
                gamelaunch.reproc_bump()
                return True, ("intensity -> %.3f (live; frozen frame "
                              "reprocessing)" % intensity)
            return True, "intensity -> %.3f (live)" % intensity
        if gamelaunch.freeze_active():
            gamelaunch.reproc_bump()
            return True, ("intensity -> %.3f (live; frozen frame "
                          "reprocessing)" % intensity)
        return True, "intensity -> %.3f" % intensity

    def set_local_tone(self, val, exe=None):
        val = max(0.0, min(2.0, float(val)))
        self.cfg.set_phase30({"local_tone": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_local_tone(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "local_tone -> %.2f" % val

    def set_local_structure(self, val, exe=None):
        val = max(0.0, min(2.0, float(val)))
        self.cfg.set_phase30({"local_structure": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_local_structure(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "local_structure -> %.2f" % val

    def set_skin_structure(self, val, exe=None):
        val = max(-1.0, min(2.0, float(val)))
        self.cfg.set_phase30({"skin_structure": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_skin_structure(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "skin_structure -> %.2f" % val

    def set_auto_mask(self, val, exe=None):
        val = bool(val)
        self.cfg.set_phase30({"auto_mask": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_auto_mask(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "auto_mask -> %s" % ("ON" if val else "OFF")

    def set_style_index(self, val, exe=None):
        val = int(val)
        val = min(max(val, 0), 2)
        self.cfg.set_phase30({"style_index": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_style_index(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "style_index -> %d" % val

    def set_color_strength(self, val, exe=None):
        val = max(0.0, min(2.0, float(val)))
        self.cfg.set_phase30({"color_strength": val}, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_color_strength(val)
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "color_strength -> %.2f" % val

    def set_phase30(self, params, exe=None):
        self.cfg.set_phase30(params, exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            for k, v in params.items():
                try:
                    if k == "intensity":
                        self.client.set_intensity(float(v))
                    elif k == "gain":
                        self.client.set_gain(float(v))
                    elif k == "local_tone":
                        self.client.set_local_tone(float(v))
                    elif k == "local_structure":
                        self.client.set_local_structure(float(v))
                    elif k == "skin_structure":
                        self.client.set_skin_structure(float(v))
                    elif k == "auto_mask":
                        self.client.set_auto_mask(bool(v))
                    elif k == "style_index":
                        self.client.set_style_index(int(v))
                    elif k == "color_strength":
                        self.client.set_color_strength(float(v))
                except Exception:
                    pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "Phase30 parameters updated"

    def reset_phase30(self, exe=None):
        self.cfg.reset_phase30(exe=exe)
        p30 = self.cfg.get_phase30(exe)
        os.environ.update(phase30_env_dict(p30))
        write_phase30_knobs(self._knob_blob(p30))
        if self.daemon.running:
            try:
                self.client.set_intensity(p30["intensity"])
            except Exception:
                pass
        if gamelaunch.freeze_active():
            # AFTER the NRCT push: a resend must see the new value.
            gamelaunch.reproc_bump()
        return True, "Phase30 parameters reset to defaults"

    # ---------------------------------------------------------- screen mode --
    def screen_start(self, gain=None):
        g = self.cfg.get("gain") if gain is None else gain
        notes = []
        if self.daemon.running:
            # VRAM EXCLUSIVITY (see daemon_start): m8blive needs its own
            # ~11.7 GB arena; keeping m11d up would OOM the card.
            ok, msg = self.daemon.stop()
            notes.append("daemon stopped first (%s)" % msg)
        extra = (self.cfg.get("screen_args") or "").split()
        scale = float(self.cfg.get("scale") or 0.50)
        framegen = bool(self.cfg.get("frame_gen"))
        ok, msg = self.screen.start(gain=g, blend=float(
            self.cfg.get("blend") or 1.0),
            scale=scale, framegen=framegen,
            extra_args=extra)
        if notes:
            msg = " | ".join(notes + [msg])
        return ok, msg

    def screen_stop(self):
        return self.screen.stop()

    # --------------------------------------------------- processing toggle --
    def processing_toggle(self):
        """Pause/resume the ACTIVE path (file channels only, no input
        injection). Pause = passthrough at full fps; the last processed
        frame is what the owner just saw."""
        mode = self.snapshot().get("active_mode")
        if mode in ("dx9", "dx11", "dx12", "vulkan"):
            if gamelaunch.mode_paused(mode):
                return gamelaunch.mode_resume(mode)
            return gamelaunch.mode_pause(mode)
        if mode == "screen":
            return False, ("screen mode: the overlay owns CTRL+ALT+X "
                           "(hide/show) itself")
        return False, "nothing to pause - no game or screen mode running"

    def processing_paused(self):
        snap = self.snapshot()
        mode = snap.get("active_mode")
        if mode in ("dx9", "dx11", "dx12", "vulkan"):
            return gamelaunch.mode_paused(mode)
        return None      # screen/none: not file-controllable

    def active_game_pid(self):
        """Pid of the currently running managed game (for freeze/resume)."""
        exe = self.snapshot().get("active_game")
        if exe:
            return find_pid(os.path.basename(exe))
        return None

    def active_game_exe(self):
        return self._active_game_exe

    # ----------------------------------------------------------- game mgmt --
    def add_game(self, exe, mode=None, arch=None, name=None, launch_exe=None):
        """Register a game exe; mode/arch auto-detected from the PE imports
        when not given. launch_exe: an optional launcher wrapper to START
        instead of the game binary (pirate/GOG/Rockstar stubs). Returns
        (ok, message)."""
        from . import gamescan
        exe = os.path.abspath(exe)
        if not os.path.exists(exe):
            return False, "game exe not found: %s" % exe
        det_arch, apis = gamescan.pe_info(exe)
        arch = arch or det_arch or "x64"
        if mode is None:
            mode = next((m for m in gamescan.API_PRIORITY if m in apis),
                        None)
        if mode is None:
            return False, ("no D3D imports found in %s - pick the mode "
                           "manually" % os.path.basename(exe))
        games = dict(self.cfg.get("games") or {})
        meta = {"mode": mode, "arch": arch,
                "name": name or os.path.splitext(os.path.basename(exe))[0]}
        if launch_exe and os.path.abspath(launch_exe).lower() != exe.lower():
            if os.path.exists(launch_exe):
                meta["launch_exe"] = os.path.abspath(launch_exe)
        games[exe] = meta
        self.cfg.set("games", games)
        via = (" (launch via %s)" % os.path.basename(meta["launch_exe"])
               if meta.get("launch_exe") else "")
        return True, "%s: %s/%s (%s)%s" % (
            meta["name"], mode.upper(), arch,
            ", ".join(apis) or "no D3D imports - manual mode", via)

    def remove_game(self, exe):
        games = dict(self.cfg.get("games") or {})
        if games.pop(exe, None) is not None:
            self.cfg.set("games", games)
            return True, "removed %s" % os.path.basename(exe)
        return False, "not in the list"

    def _ensure_daemon_stopped(self, log_fn=None):
        def _log(msg):
            if log_fn:
                log_fn(msg)
            print(msg, flush=True)
        pid = self.daemon.pid
        if pid is None:
            return True, "daemon not running"
        ok, msg = self.daemon.stop()
        if ok and not self.daemon.running:
            return True, "stopped x86 daemon before x64 launch"
        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            if not self.daemon.running:
                return True, "x86 daemon exited after stop signal"
            time.sleep(0.3)
        _log("[controller] graceful daemon stop timed out; force-killing pid %s" % self.daemon.pid)
        target_pid = self.daemon.pid
        if target_pid is not None:
            from .processes import stop_pid
            stop_pid(target_pid, force=True)
        deadline2 = time.monotonic() + 3.0
        while time.monotonic() < deadline2:
            if not self.daemon.running:
                return True, "force-killed x86 daemon before x64 launch"
            time.sleep(0.3)
        if self.daemon.running:
            return False, "cannot release x86 daemon before x64 game: daemon still alive after force-kill"
        return True, "force-killed x86 daemon before x64 launch"

    def game_deploy(self, exe):
        meta = (self.cfg.get("games") or {}).get(exe)
        if not meta:
            return False, "add the game first"
        removed = []
        try:
            dep = Deployer(os.path.dirname(exe))
            removed = dep.undeploy_other_modes(meta["mode"], meta["arch"])
            out = dep.deploy_mode(meta["mode"], meta["arch"],
                                  target_name=os.path.basename(exe))
        except deploy_mod.DeployError as e:
            return False, str(e)
        if not out:
            return True, ("%s enabled (Vulkan game - the registered layer "
                          "loads by itself, no DLLs needed)" % meta["name"])
        bad = {d: s for d, s in out.items() if s != "deployed"}
        if bad:
            return False, "deploy incomplete: %s" % bad
        tail = ""
        if removed:
            tail = " (removed stale: %s)" % ", ".join(removed)
        return True, "%s enabled (%s: %s)%s" % (
            meta["name"], meta["mode"].upper(), ", ".join(out), tail)

    def game_undeploy(self, exe):
        meta = (self.cfg.get("games") or {}).get(exe)
        if not meta:
            return False, "add the game first"
        try:
            Deployer(os.path.dirname(exe)).undeploy_mode(meta["mode"],
                                                         meta["arch"])
        except deploy_mod.DeployError as e:
            return False, str(e)
        return True, "%s disabled, originals restored" % meta["name"]

    def game_launch(self, exe):
        """Synchronous launch (kept for tests/scripts). The UI calls
        launch_game_async so a blocked step never stalls the ActionWorker
        queue."""
        return self.launcher.launch(exe)

    def launch_game_async(self, exe, register=None):
        """UI path: launches run on their own thread with their own guard,
        off the shared action queue."""
        if self.launcher.busy():
            self._report_result(False,
                                "a game launch is already in progress")
            return

        def _run():
            ok, msg = self.launcher.launch(exe, register=register)
            self._report_result(ok, msg)

        threading.Thread(target=_run, daemon=True, name="m13-launch").start()

    def _report_result(self, ok, msg):
        w = self.worker
        if w and getattr(w, "on_result", None):
            try:
                w.on_result(ok, msg)
                return
            except Exception:
                pass
        print(msg, flush=True)

    def note_launched(self, exe, target):
        """Bookkeeping after a successful spawn (was inlined in game_launch)."""
        self.launched_exes.add(os.path.abspath(target).lower())
        self._active_game_exe = exe

    # ------------------------------------------------------- freeze support --
    def freeze_supported(self):
        """True when the ACTIVE game understands the freeze/reprocess flags.
        Supported for all active games in dx9, dx11, dx12, vulkan."""
        snap = self.snapshot()
        mode = snap.get("active_mode")
        return mode in ("dx9", "dx11", "dx12", "vulkan")

    # ------------------------------------------------------------ registry --
    def layers_status(self):
        return {
            "x64": LayerRegistry.registered(deploy_mod.MANIFEST_X64),
            "x86": LayerRegistry.registered(deploy_mod.MANIFEST_X86),
        }

    def layers_register(self):
        """Register OUR manifests and unregister the other layout's copies
        (dev vs production) - two registered dlssnr layers would both patch
        presents and double-process every frame."""
        own = {deploy_mod.MANIFEST_X64, deploy_mod.MANIFEST_X86}
        removed = 0
        for key in ("layer_x64", "layer_x86"):
            for cand in paths.find_all(key):
                if cand not in own and LayerRegistry.registered(cand):
                    LayerRegistry.unregister(cand)
                    removed += 1
        LayerRegistry.register(deploy_mod.MANIFEST_X64)
        LayerRegistry.register(deploy_mod.MANIFEST_X86)
        return True, ("layers registered; removed %d other-copy "
                      "registration(s)") % removed

    def layers_unregister(self):
        LayerRegistry.unregister(deploy_mod.MANIFEST_X64)
        LayerRegistry.unregister(deploy_mod.MANIFEST_X86)
        return True, "layer manifests unregistered"
