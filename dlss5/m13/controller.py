# ============================================================================
# m13.controller - the glue between the UI and the core modules. All side
# effects (processes, TCP, files, registry) go through here; widgets stay
# dumb. Every method returns (ok, message) so the UI can log/flash it.
# ============================================================================
import os

from . import config as config_mod
from . import deploy as deploy_mod
from . import gamelaunch
from . import paths
from .daemonctl import M11dClient, DaemonError
from .deploy import Deployer, LayerRegistry, DXVK_X32_D3D9, M12_PROXY
from .processes import ManagedProcess
from .screenmode import ScreenMode

M11D_EXE = paths.find("m11d_exe")
M11D_CWD = os.path.dirname(M11D_EXE)
M11D_LOG = os.path.join(paths.find("logs"), "m11d.log")


class M13Controller:
    def __init__(self, cfg=None):
        self.cfg = cfg or config_mod.Config()
        os.makedirs(os.path.dirname(M11D_LOG), exist_ok=True)
        self.daemon = ManagedProcess("m11d.exe", M11D_EXE, M11D_CWD, M11D_LOG)
        self.screen = ScreenMode()
        self.client = M11dClient()

    # ------------------------------------------------------------- daemon --
    def weights_ok(self):
        p = self.cfg.get("weights_path")
        return bool(p) and os.path.exists(p)

    def daemon_status(self):
        """-> dict(running, gain, frames, error). Never raises."""
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
        """Live gain push (m11d: next frame, no restart). Always persisted."""
        self.cfg.set("gain", gain)
        if not self.daemon.running:
            return False, "daemon down - gain saved for next start"
        try:
            g, _ = self.client.set_gain(gain)
            return True, "gain -> %.3f (live)" % g
        except DaemonError as e:
            return False, "gain saved but daemon push failed: %s" % e

    # ---------------------------------------------------------- screen mode --
    def screen_start(self, gain=None):
        g = self.cfg.get("gain") if gain is None else gain
        extra = (self.cfg.get("screen_args") or "").split()
        return self.screen.start(gain=g, extra_args=extra)

    def screen_stop(self):
        return self.screen.stop()

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
