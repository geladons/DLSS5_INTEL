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
import queue
import threading

from . import config as config_mod
from . import deploy as deploy_mod
from . import gamelaunch
from . import paths
from .daemonctl import M11dClient, DaemonError
from .deploy import Deployer, LayerRegistry
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
        snap["active_mode"] = self._active_mode(snap)
        snap["active_game"] = self._active_game(snap)
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


# ------------------------------------------------------------ async jobs ---
class ActionWorker(threading.Thread):
    """One sequential worker for every mutating action (daemon start/stop,
    gain push, deploy, pause/resume). Keeps the Tk thread at 0 ms of I/O."""

    def __init__(self, on_result):
        super().__init__(daemon=True, name="m13-actions")
        self.on_result = on_result        # called from THIS thread
        self.q = queue.Queue()
        self._stop_ev = threading.Event()

    def submit(self, fn, args=()):
        self.q.put((fn, args))

    def stop(self):
        self._stop_ev.set()
        self.q.put((None, ()))

    def run(self):
        while not self._stop_ev.is_set():
            fn, args = self.q.get()
            if fn is None:
                continue
            try:
                ok, msg = fn(*args)
            except Exception as e:
                ok, msg = False, "error: %s" % e
            try:
                self.on_result(ok, msg)
            except Exception:
                pass


# -------------------------------------------------------------- controller --
class M13Controller:
    def __init__(self, cfg=None):
        self.cfg = cfg or config_mod.Config()
        self._migrate_legacy_games()
        os.makedirs(os.path.dirname(M11D_LOG), exist_ok=True)
        self.daemon = ManagedProcess("m11d.exe", M11D_EXE, M11D_CWD, M11D_LOG)
        self.screen = ScreenMode()
        self.client = M11dClient(timeout=NRCT_TIMEOUT)
        self.monitor = StateMonitor(self)
        self.worker = None          # created by the UI (needs its callback)
        self.launched_exes = set()  # launch targets started BY US this
                                    # session (they carry the layer freeze env)
        self.monitor.start()

    def _migrate_legacy_games(self):
        """v1 config had single dx9_game/dx12_game keys -> games dict.
        v3 -> v4: entries pointing at launcher STUBS (the owner added
        Launcher.exe / the GOG stub as the game) are retargeted to the real
        renderer binary; the stub becomes launch_exe so the crack chain
        still runs. Dead paths are dropped."""
        games = dict(self.cfg.get("games") or {})
        changed = False
        for key, mode in (("dx9_game", "dx9"), ("dx12_game", "dx12")):
            exe = self.cfg.get(key)
            if exe and exe not in games:
                games[exe] = {"mode": mode,
                              "arch": "x86" if mode == "dx9" else "x64",
                              "name": os.path.splitext(
                                  os.path.basename(exe))[0]}
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
        if changed:
            self.cfg.set("games", games)

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
        if self.weights_ok() and not self.daemon.running:
            ok, msg = self.daemon_start()
            steps.append("auto: %s" % msg)
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

    def shutdown(self):
        self.monitor.stop()
        if self.worker:
            self.worker.stop()

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
        args = ["--port", "47990", "--gain", "%.3f" % self.cfg.get("gain"),
                "--blend", "%.3f" % float(self.cfg.get("blend") or 1.0),
                "--weights", self.cfg.get("weights_path")]
        ok, msg = self.daemon.start(args)
        if notes:
            msg = " | ".join(notes + [msg])
        return ok, msg

    def daemon_stop(self):
        return self.daemon.stop()

    # --------------------------------------------------------------- gain --
    def set_gain(self, gain):
        """Live gain push. Daemon up: NRCT, next processed frame; if a game
        frame is FROZEN (photo mode) the layer/proxy is asked to reprocess
        the held raw frame so the frozen picture updates. Screen mode up:
        the knob FILE retunes m8blive live (no restart - a restart costs
        the ~30 s weights upload and looks like a crash). Always persisted."""
        gain = max(0.0, min(16.0, float(gain)))
        self.cfg.set("gain", gain)
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
            write_knobs(gain, float(self.cfg.get("blend")))
            return True, "gain -> %.3f (live, screen mode)" % gain
        return False, "daemon down - gain saved for next start"

    def set_blend(self, blend):
        """Live blend push (vendor mix factor 0..1). Same routing as gain."""
        blend = max(0.0, min(1.0, float(blend)))
        self.cfg.set("blend", blend)
        if self.daemon.running:
            try:
                self.client.set_blend(blend)
            except DaemonError as e:
                return False, "blend saved but daemon push failed: %s" % e
            if gamelaunch.freeze_active():
                gamelaunch.reproc_bump()
                return True, ("blend -> %.3f (live; frozen frame "
                              "reprocessing)" % blend)
            return True, "blend -> %.3f (live)" % blend
        if self.screen.running:
            from .screenmode import write_knobs
            write_knobs(float(self.cfg.get("gain")), blend)
            return True, "blend -> %.3f (live, screen mode)" % blend
        return False, "daemon down - blend saved for next start"

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
        ok, msg = self.screen.start(gain=g, blend=float(
            self.cfg.get("blend") or 1.0), extra_args=extra)
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

    @staticmethod
    def launch_target(exe, meta=None):
        """What 'Launch' actually starts (the wrapper when registered)."""
        meta = meta or {}
        target = meta.get("launch_exe") or exe
        return target if os.path.exists(target) else exe

    def game_deploy(self, exe):
        meta = (self.cfg.get("games") or {}).get(exe)
        if not meta:
            return False, "add the game first"
        try:
            out = Deployer(os.path.dirname(exe)).deploy_mode(meta["mode"],
                                                             meta["arch"])
        except deploy_mod.DeployError as e:
            return False, str(e)
        if not out:
            return True, ("%s enabled (Vulkan game - the registered layer "
                          "loads by itself, no DLLs needed)" % meta["name"])
        bad = {d: s for d, s in out.items() if s != "deployed"}
        if bad:
            return False, "deploy incomplete: %s" % bad
        return True, "%s enabled (%s: %s)" % (
            meta["name"], meta["mode"].upper(), ", ".join(out))

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
        meta = (self.cfg.get("games") or {}).get(exe)
        if not meta:
            return False, "add the game first"
        d = Deployer(os.path.dirname(exe))
        if d.mode_status(meta["mode"], meta["arch"]) != "deployed":
            ok, msg = self.game_deploy(exe)     # auto-deploy on launch
            if not ok:
                return False, msg
        if not self.daemon.running:
            ok, msg = self.daemon_start()       # auto-start the chain too
            if not ok and "already" not in msg:
                return False, msg
        target = self.launch_target(exe, meta)
        ok, msg = gamelaunch.launch_game(target, meta["mode"])
        if ok:
            self.launched_exes.add(os.path.abspath(target).lower())
            if target.lower() != exe.lower():
                msg = "%s (via %s)" % (msg, os.path.basename(target))
        return ok, msg

    # ------------------------------------------------------- freeze support --
    def freeze_supported(self):
        """True when the ACTIVE game understands the freeze/reprocess flags:
        always for DX12 (the proxy reads fixed %TEMP% names); for the Vulkan
        layer modes only when we launched the game (it carries the env)."""
        snap = self.snapshot()
        mode = snap.get("active_mode")
        if mode == "dx12":
            return True
        if mode not in ("dx9", "dx11", "vulkan"):
            return False
        exe = snap.get("active_game")
        meta = (self.cfg.get("games") or {}).get(exe or "") or {}
        target = os.path.abspath(self.launch_target(exe, meta)).lower() \
            if exe else None
        return bool(target) and target in self.launched_exes

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
