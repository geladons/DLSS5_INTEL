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
from .deploy import Deployer, LayerRegistry, DXVK_X32_D3D9, M12_PROXY
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
        # games from cfg (basename probe, no UI var access)
        for mode, key in (("dx9", "dx9_game"), ("dx12", "dx12_game")):
            exe = (ctl.cfg.get(key) or "").strip()
            snap[mode + "_exe"] = exe
            snap[mode + "_running"] = bool(exe) and \
                find_pid(os.path.basename(exe)) is not None
            snap[mode + "_paused"] = (gamelaunch.dx9_paused() if mode == "dx9"
                                      else gamelaunch.dx12_paused())
            snap[mode + "_dll"] = None
            snap[mode + "_guards"] = []
            if exe and os.path.isdir(os.path.dirname(exe)):
                try:
                    if mode == "dx9":
                        st = ctl.dx9_status(exe)
                    else:
                        st = ctl.dx12_status(exe)
                    snap[mode + "_dll"] = st.get("dll")
                    snap[mode + "_guards"] = st.get("guards", [])
                except Exception as e:
                    snap[mode + "_guards"] = ["probe failed: %s" % e]
        snap["weights_ok"] = ctl.weights_ok()
        snap["active_mode"] = self._active_mode(snap)
        return snap

    @staticmethod
    def _active_mode(snap):
        """Which processing path is live right now (pause/gain target)."""
        if snap.get("dx12_running"):
            return "dx12"
        if snap.get("dx9_running"):
            return "dx9"
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
        os.makedirs(os.path.dirname(M11D_LOG), exist_ok=True)
        self.daemon = ManagedProcess("m11d.exe", M11D_EXE, M11D_CWD, M11D_LOG)
        self.screen = ScreenMode()
        self.client = M11dClient(timeout=NRCT_TIMEOUT)
        self.monitor = StateMonitor(self)
        self.worker = None          # created by the UI (needs its callback)
        self.monitor.start()

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
        args = ["--port", "47990", "--gain", "%.3f" % self.cfg.get("gain"),
                "--weights", self.cfg.get("weights_path")]
        return self.daemon.start(args)

    def daemon_stop(self):
        return self.daemon.stop()

    # --------------------------------------------------------------- gain --
    def set_gain(self, gain):
        """Live gain push. Daemon up: NRCT, next processed frame. Daemon down
        but screen mode up: restart m8blive with the new gain (cheap, no game
        attached). Always persisted."""
        gain = max(0.0, min(16.0, float(gain)))
        self.cfg.set("gain", gain)
        if self.daemon.running:
            try:
                g, _ = self.client.set_gain(gain)
                return True, "gain -> %.3f (live)" % g
            except DaemonError as e:
                return False, "gain saved but daemon push failed: %s" % e
        if self.screen.running:
            ok, msg = self.screen.stop()
            if not ok:
                return False, "gain saved; screen restart failed: %s" % msg
            ok, msg = self.screen_start(gain)
            return ok, "gain %.3f - screen overlay restarted (%s)" % (gain, msg)
        return False, "daemon down - gain saved for next start"

    # ---------------------------------------------------------- screen mode --
    def screen_start(self, gain=None):
        g = self.cfg.get("gain") if gain is None else gain
        extra = (self.cfg.get("screen_args") or "").split()
        return self.screen.start(gain=g, extra_args=extra)

    def screen_stop(self):
        return self.screen.stop()

    # --------------------------------------------------- processing toggle --
    def processing_toggle(self):
        """Pause/resume the ACTIVE path (file channels only, no input
        injection). Pause = passthrough at full fps; the last processed
        frame is what the owner just saw."""
        mode = self.snapshot().get("active_mode")
        if mode == "dx9":
            return gamelaunch.dx9_resume() if gamelaunch.dx9_paused() \
                else gamelaunch.dx9_pause()
        if mode == "dx12":
            return gamelaunch.dx12_resume() if gamelaunch.dx12_paused() \
                else gamelaunch.dx12_pause()
        if mode == "screen":
            return False, ("screen mode: the overlay owns CTRL+ALT+X "
                           "(hide/show) itself")
        return False, "nothing to pause - no game or screen mode running"

    def processing_paused(self):
        snap = self.snapshot()
        mode = snap.get("active_mode")
        if mode in ("dx9", "dx12"):
            return snap.get(mode + "_paused")
        return None      # screen/none: not file-controllable

    # ------------------------------------------------------------ DX9 path --
    def dx9_status(self, game_exe):
        if not game_exe:
            return {"game": None}
        d = Deployer(os.path.dirname(game_exe))
        return {
            "game": game_exe,
            "dll": d.status("d3d9.dll", DXVK_X32_D3D9),
            "guards": deploy_mod.dx9_guard(os.path.dirname(game_exe)),
            "paused": gamelaunch.dx9_paused(),
        }

    def dx9_deploy(self, game_exe):
        if not game_exe:
            return False, "pick the game exe first"
        d = Deployer(os.path.dirname(game_exe))
        try:
            ok = d.deploy("d3d9.dll", DXVK_X32_D3D9)
            return ok, "d3d9.dll deployed (dxvk x32, dxgi.dll NOT touched)"
        except deploy_mod.DeployError as e:
            return False, str(e)

    def dx9_undeploy(self, game_exe):
        if not game_exe:
            return False, "pick the game exe first"
        d = Deployer(os.path.dirname(game_exe))
        try:
            d.undeploy("d3d9.dll", DXVK_X32_D3D9)
            return True, "d3d9.dll removed, original restored if any"
        except deploy_mod.DeployError as e:
            return False, str(e)

    def dx9_launch(self, game_exe, live_every=1):
        if not game_exe:
            return False, "pick the game exe first"
        st = self.dx9_status(game_exe)
        if st.get("dll") != "deployed":
            return False, "deploy d3d9.dll first"
        return gamelaunch.launch_dx9(game_exe, live_every)

    def dx9_pause(self):
        return gamelaunch.dx9_pause()

    def dx9_resume(self):
        return gamelaunch.dx9_resume()

    # ----------------------------------------------------------- DX12 path --
    def dx12_status(self, game_exe):
        if not game_exe:
            return {"game": None}
        d = Deployer(os.path.dirname(game_exe))
        return {
            "game": game_exe,
            "dll": d.status("dxgi.dll", M12_PROXY),
            "paused": gamelaunch.dx12_paused(),
        }

    def dx12_deploy(self, game_exe):
        if not game_exe:
            return False, "pick the game exe first"
        d = Deployer(os.path.dirname(game_exe))
        try:
            ok = d.deploy("dxgi.dll", M12_PROXY)
            return ok, ("dxgi.dll proxy deployed; NOTE: game must be RESTARTED, "
                        "anti-cheat may block it (see DEV_STATE BattleEye note)")
        except deploy_mod.DeployError as e:
            return False, str(e)

    def dx12_undeploy(self, game_exe):
        if not game_exe:
            return False, "pick the game exe first"
        d = Deployer(os.path.dirname(game_exe))
        try:
            d.undeploy("dxgi.dll", M12_PROXY)
            return True, "dxgi.dll removed, original restored if any"
        except deploy_mod.DeployError as e:
            return False, str(e)

    def dx12_launch(self, game_exe):
        if not game_exe:
            return False, "pick the game exe first"
        st = self.dx12_status(game_exe)
        if st.get("dll") != "deployed":
            return False, "deploy the dxgi.dll proxy first"
        return gamelaunch.launch_dx12(game_exe)

    def dx12_pause(self):
        return gamelaunch.dx12_pause()

    def dx12_resume(self):
        return gamelaunch.dx12_resume()

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
