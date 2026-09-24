# ============================================================================
# m13.deploy - DLL deploy/undeploy for the injection paths + HKCU Vulkan
# layer registration.
#
# HARD-WON RULES (docs/HANDOFF_GTA4_DX9.md, DEV_STATE.md - do not rediscover):
#  - DX9 games get ONLY dxvk x32 d3d9.dll next to the game exe. NEVER
#    dxgi.dll: system d3d11 + DXVK dxgi = APPCRASH in GTA IV.
#  - HKCU registry writes go through winreg, NOT reg.exe (inline quoting
#    through shells mangles quotes and created a bogus registry key once).
#  - Game dirs may be ACL-restricted (GTA5: copy needs elevation) and DLLs
#    are locked while the game runs - both surface as PermissionError/OSError.
# ============================================================================
import hashlib
import os
import shutil
import winreg

from . import paths

# Runtime artifacts: production bundle layout first, dev tree fallback
# (see paths.py - the same code serves both).
DXVK_X32_D3D9 = paths.find("dxvk_d3d9")
M12_PROXY = paths.find("m12_proxy")
MANIFEST_X64 = paths.find("layer_x64")
MANIFEST_X86 = paths.find("layer_x86")

LAYERS_KEY = r"Software\Khronos\Vulkan\ImplicitLayers"
BACKUP_SUFFIX = ".m13bak"


class DeployError(Exception):
    pass


def _sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------- registry --
class LayerRegistry:
    """HKCU Vulkan ImplicitLayers management via winreg (no reg.exe)."""

    @staticmethod
    def _open():
        return winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, LAYERS_KEY, 0,
                                  winreg.KEY_READ | winreg.KEY_SET_VALUE)

    @staticmethod
    def registered(manifest_path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, LAYERS_KEY) as k:
                winreg.QueryValueEx(k, manifest_path)
                return True
        except OSError:
            return False

    @staticmethod
    def register(manifest_path):
        if not os.path.exists(manifest_path):
            raise DeployError("manifest not found: %s" % manifest_path)
        with LayerRegistry._open() as k:
            winreg.SetValueEx(k, manifest_path, 0, winreg.REG_DWORD, 0)

    @staticmethod
    def unregister(manifest_path):
        try:
            with LayerRegistry._open() as k:
                winreg.DeleteValue(k, manifest_path)
        except OSError:
            pass   # not registered

    @staticmethod
    def registered_paths():
        out = []
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, LAYERS_KEY) as k:
                i = 0
                while True:
                    try:
                        name, _, _ = winreg.EnumValue(k, i)
                        out.append(name)
                        i += 1
                    except OSError:
                        break
        except OSError:
            pass
        return out

    @staticmethod
    def layers_ready():
        return (LayerRegistry.registered(MANIFEST_X64)
                and LayerRegistry.registered(MANIFEST_X86))


# ---------------------------------------------------------------- deploy ---
class Deployer:
    """Copy/remove the right DLL next to a game exe, with backup+restore."""

    def __init__(self, game_dir):
        self.game_dir = game_dir

    def _target(self, dll_name):
        return os.path.join(self.game_dir, dll_name)

    def _backup(self, dll_name):
        return self._target(dll_name) + BACKUP_SUFFIX

    def status(self, dll_name, source_path):
        """-> 'deployed' | 'backup_only' | 'foreign' | 'clean'"""
        tgt = self._target(dll_name)
        if os.path.exists(tgt):
            if os.path.exists(source_path) and _sha1(tgt) == _sha1(source_path):
                return "deployed"
            if os.path.exists(self._backup(dll_name)):
                return "backup_only"   # foreign dll parked over our backup?
            return "foreign"
        if os.path.exists(self._backup(dll_name)):
            return "backup_only"
        return "clean"

    def deploy(self, dll_name, source_path, extra_guard=None):
        """Copy source_path to game_dir/dll_name; back up a pre-existing one.
        extra_guard: optional callable raised warnings list (DX9 dxgi rule)."""
        if not os.path.exists(source_path):
            raise DeployError("source DLL missing: %s" % source_path)
        if not os.path.isdir(self.game_dir):
            raise DeployError("game dir not found: %s" % self.game_dir)
        tgt = self._target(dll_name)
        bak = self._backup(dll_name)
        try:
            if os.path.exists(tgt) and not os.path.exists(bak):
                st = self.status(dll_name, source_path)
                if st != "deployed":
                    shutil.copy2(tgt, bak)          # park the game's own dll
            shutil.copy2(source_path, tgt)
        except OSError as e:
            raise DeployError("copy failed (%s) - game running or needs "
                              "elevation?" % e) from e
        return self.status(dll_name, source_path) == "deployed"

    def undeploy(self, dll_name, source_path):
        """Remove our DLL; restore the backed-up original if present."""
        tgt = self._target(dll_name)
        bak = self._backup(dll_name)
        try:
            if os.path.exists(tgt):
                if (os.path.exists(source_path)
                        and _sha1(tgt) == _sha1(source_path)):
                    os.remove(tgt)
                else:
                    raise DeployError("%s was replaced by something else - "
                                      "leaving it in place" % tgt)
            if os.path.exists(bak):
                os.replace(bak, tgt)   # restore the game's own dll
        except OSError as e:
            raise DeployError("undeploy failed (%s) - game running?" % e
                              ) from e
        return self.status(dll_name, source_path)


def dx9_guard(game_dir):
    """DX9 rule enforcement: NEVER a dxgi.dll from DXVK next to a D3D9 game.
    Returns a list of human warnings ([] = clear to deploy)."""
    warnings = []
    dxgi = os.path.join(game_dir, "dxgi.dll")
    if os.path.exists(dxgi):
        try:
            with open(dxgi, "rb") as f:
                head = f.read(64)
            # DXVK builds carry a 'DXVK' marker; the .gta4.bak file proves
            # this class of crash happened once already.
            if b"DXVK" in head or os.path.exists(dxgi + ".gta4.bak"):
                warnings.append("dxgi.dll in the game dir looks like DXVK - "
                                "GTA IV crashed on that mix (system d3d11 + "
                                "DXVK dxgi). Remove it or the game may die.")
        except OSError:
            warnings.append("dxgi.dll exists in the game dir - not touched, "
                            "but for D3D9 games only d3d9.dll may come from "
                            "DXVK.")
    return warnings
