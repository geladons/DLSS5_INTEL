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

# Injection modes: which DLLs land next to the game exe.
#  dx9  - DXVK d3d9.dll ONLY (the dxgi-from-DXVK + system d3d11 mix killed
#         GTA IV - never deploy dxgi.dll for a D3D9 game)
#  dx11 - DXVK d3d11.dll + dxgi.dll (+d3d10core.dll: system d3d10.dll
#         forwards to it, that is how DX10 rides the same path)
#  dx12 - our own m12 dxgi.dll proxy (no DXVK involved)
MODE_DLLS = {
    "dx9":    ("d3d9.dll",),
    "dx11":   ("d3d11.dll", "dxgi.dll", "d3d10core.dll"),
    "dx12":   ("dxgi.dll",),
    "vulkan": (),     # nothing to deploy: the implicit layer self-loads
}


def mode_dll_source(mode, arch, dll_name):
    """Source path for one dll of a mode. arch: 'x64' | 'x86'."""
    if mode == "dx12":
        return M12_PROXY
    dxvk_dir = paths.find("dxvk_x64" if arch == "x64" else "dxvk_x32")
    return os.path.join(dxvk_dir, dll_name)


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
        except PermissionError:
            raise DeployNeedsElevation(dll_name)
        except OSError as e:
            raise DeployError("copy failed (%s) - game running?" % e) from e
        return self.status(dll_name, source_path) == "deployed"

    def deploy_mode(self, mode, arch):
        """Deploy every dll of an injection mode; auto-elevates on ACL
        dirs (one UAC prompt copies them all). Returns status dict."""
        if mode not in MODE_DLLS:
            raise DeployError("unknown mode: %s" % mode)
        if mode == "dx9":
            warns = dx9_guard(self.game_dir)
            if warns:
                raise DeployError(" ".join(warns))
        out = {}
        need_elevation = []
        for dll in MODE_DLLS[mode]:
            src = mode_dll_source(mode, arch, dll)
            try:
                self.deploy(dll, src)
                out[dll] = "deployed"
            except DeployNeedsElevation:
                need_elevation.append((src, dll))
        if need_elevation:
            _elevated_copy(self.game_dir, need_elevation)
            for _src, dll in need_elevation:
                out[dll] = self.status(dll, mode_dll_source(mode, arch, dll))
        return out

    def undeploy_mode(self, mode, arch):
        """Remove every dll of a mode, restoring originals."""
        for dll in MODE_DLLS.get(mode, ()):
            try:
                self.undeploy(dll, mode_dll_source(mode, arch, dll))
            except PermissionError:
                _elevated_remove(self.game_dir, dll)
        return True

    def mode_status(self, mode, arch):
        """-> 'deployed' | 'partial' | 'foreign' | 'clean' for a mode."""
        states = [self.status(dll, mode_dll_source(mode, arch, dll))
                  for dll in MODE_DLLS.get(mode, ())]
        if not states:
            return "deployed"       # vulkan: nothing to deploy
        if all(s == "deployed" for s in states):
            return "deployed"
        if any(s == "deployed" for s in states):
            return "partial"
        if any(s == "foreign" for s in states):
            return "foreign"
        return "clean"

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


class DeployNeedsElevation(DeployError):
    """The game dir is ACL-protected (Program Files, launcher-owned)."""


# --------------------------------------------------------- elevated copy ---
_PS_COPY = r"""
param([string]$Json)
$pairs = $Json | ConvertFrom-Json
foreach ($p in $pairs) {
    $dst = $p.dst; $bak = $dst + '.m13bak'
    if ((Test-Path $dst) -and -not (Test-Path $bak)) {
        Copy-Item $dst $bak -Force
    }
    Copy-Item $p.src $dst -Force
}
"""

_PS_REMOVE = r"""
param([string]$Dst)
$bak = $Dst + '.m13bak'
if (Test-Path $Dst) { Remove-Item $Dst -Force }
if (Test-Path $bak) { Move-Item $bak $Dst -Force }
"""


def _run_elevated(ps_body, arg):
    """Run a small PowerShell script elevated (one UAC prompt), wait for it.
    Quoting gotcha: the script goes to a temp .ps1, the arg is JSON."""
    import json as _json
    import subprocess
    import tempfile
    from .processes import CREATE_NO_WINDOW
    fd, script = tempfile.mkstemp(suffix=".ps1", prefix="m13elev_")
    with os.fdopen(fd, "w") as f:
        f.write(ps_body)
    argfile = script + ".json"
    with open(argfile, "w") as f:
        _json.dump(arg, f)
    inner = ('& "%s" -Json (Get-Content -Raw "%s")'
             % (script.replace('"', '`"'), argfile.replace('"', '`"')))
    cmd = ["powershell", "-NoProfile", "-Command",
           "Start-Process", "powershell", "-Verb", "RunAs", "-Wait",
           "-WindowStyle", "Hidden", "-ArgumentList",
           "'-NoProfile','-ExecutionPolicy','Bypass','-Command',\"%s\"" % inner]
    try:
        subprocess.run(cmd, timeout=120, creationflags=CREATE_NO_WINDOW,
                       capture_output=True)
    finally:
        for p in (script, argfile):
            try:
                os.remove(p)
            except OSError:
                pass


def _elevated_copy(game_dir, pairs):
    """pairs: [(src, dll_name)] copied into game_dir with .m13bak backups."""
    args = [{"src": s, "dst": os.path.join(game_dir, d)} for s, d in pairs]
    _run_elevated(_PS_COPY, args)
    missing = [d for s, d in pairs
               if not os.path.exists(os.path.join(game_dir, d))]
    if missing:
        raise DeployError("elevated copy did not land: %s (UAC declined?)"
                          % ", ".join(missing))


def _elevated_remove(game_dir, dll_name):
    _run_elevated(_PS_REMOVE, os.path.join(game_dir, dll_name))


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
