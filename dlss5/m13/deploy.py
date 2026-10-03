# SPDX-License-Identifier: Apache-2.0
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
    "dx12":   ("dxgi.dll", "d5c_chain.dll"),
    "vulkan": (),     # nothing to deploy: the implicit layer self-loads
}

# Written next to the dx12 proxy: host-exe basename that may spawn the
# in-process engine. Launcher exes sharing the dir stay passthrough.
M12_TARGET_FILE = "m12_target.txt"


def mode_dll_source(mode, arch, dll_name):
    """Source path for one dll of a mode. arch: 'x64' | 'x86'."""
    if mode == "dx12":
        if dll_name == "dxgi.dll":
            return M12_PROXY
        m12_dir = os.path.dirname(M12_PROXY)
        return os.path.join(m12_dir, dll_name)
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
                # park the game's own dll - but never back up an older
                # build of OURS: restore-on-undeploy would resurrect a
                # stale proxy as if it were the game's original
                if not self._is_ours_any_mode(dll_name, source_path):
                    shutil.copy2(tgt, bak)
            shutil.copy2(source_path, tgt)
        except PermissionError:
            raise DeployNeedsElevation(dll_name)
        except OSError as e:
            raise DeployError("copy failed (%s) - game running?" % e) from e
        return self.status(dll_name, source_path) == "deployed"

    def deploy_mode(self, mode, arch, target_name=None):
        """Deploy every dll of an injection mode; auto-elevates on ACL
        dirs (one UAC prompt copies them all). Returns status dict.
        target_name: basename of the game exe - for dx12 the proxy writes
        m12_target.txt so launcher processes in the same dir stay a pure
        passthrough instead of spawning a second engine."""
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
        if mode == "dx12":
            self._write_m12_target(target_name)
        # NOTE: no dual-renderer sister-dir sync here (removed Stage 25):
        # auto-deploying our dxgi proxy into a sibling x64_dx12 dir crashed
        # Witcher 3 on the first processed present. Deploy only what was
        # asked; undeploy_mode still cleans sister dirs left by old builds.
        return out

    def _write_m12_target(self, target_name):
        """m12_target.txt = the exe the dxgi proxy may spawn an engine for.
        Plain user-writable file; if the dir is ACL-protected the deploy
        still succeeded - the gate simply stays open (legacy behavior)."""
        path = self._target(M12_TARGET_FILE)
        try:
            if target_name:
                with open(path, "w") as f:
                    f.write(target_name.strip() + "\n")
            elif os.path.exists(path):
                os.remove(path)
        except OSError:
            pass

    def undeploy_other_modes(self, mode, arch):
        """Remove OUR proxies left by OTHER injection modes (best effort).
        A game that switched dx12->vulkan (or got re-registered with a
        corrected mode) keeps the old dxgi.dll otherwise - and launcher
        exes in that dir load it and spawn a second engine (RDR2). Only
        files whose hash matches our sources are touched; foreign dlls and
        the game's own files are left alone."""
        removed = []
        for other, dlls in MODE_DLLS.items():
            if other == mode:
                continue
            for dll in dlls:
                src = mode_dll_source(other, arch, dll)
                try:
                    if self._is_ours(dll, other, src):
                        self.undeploy(dll, src, trust=True)
                        removed.append(dll)
                except (DeployError, OSError):
                    pass   # foreign or ACL-protected - leave it
        try:
            tgt = self._target(M12_TARGET_FILE)
            if mode != "dx12" and os.path.exists(tgt):
                os.remove(tgt)
                removed.append(M12_TARGET_FILE)
        except OSError:
            pass
        return removed

    def undeploy_mode(self, mode, arch):
        """Remove every dll of a mode, restoring originals."""
        for dll in MODE_DLLS.get(mode, ()):
            try:
                self.undeploy(dll, mode_dll_source(mode, arch, dll))
            except PermissionError:
                _elevated_remove(self.game_dir, dll)
        if mode == "dx12":
            try:
                tgt = self._target(M12_TARGET_FILE)
                if os.path.exists(tgt):
                    os.remove(tgt)
            except OSError:
                pass

        parent = os.path.dirname(self.game_dir)
        base = os.path.basename(self.game_dir).lower()
        if base == "x64_dx12" and os.path.isdir(os.path.join(parent, "x64")):
            try:
                Deployer(os.path.join(parent, "x64")).undeploy_mode("dx11", arch)
            except Exception:
                pass
        elif base == "x64" and os.path.isdir(os.path.join(parent, "x64_dx12")):
            try:
                Deployer(os.path.join(parent, "x64_dx12")).undeploy_mode("dx12", arch)
            except Exception:
                pass

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

    def undeploy(self, dll_name, source_path, trust=False):
        """Remove our DLL; restore the backed-up original if present.
        trust=True skips the hash check - used when provenance was already
        proven by a binary marker (_is_ours), e.g. removing an OLD build
        of our own proxy that no longer matches the current source hash."""
        tgt = self._target(dll_name)
        bak = self._backup(dll_name)
        try:
            if os.path.exists(tgt):
                if (trust or (os.path.exists(source_path)
                        and _sha1(tgt) == _sha1(source_path))):
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

    def _is_ours(self, dll_name, mode, source_path):
        """Provenance beyond the current-source hash: our binaries carry
        literal markers (m12_dxgi, DXVK, dlss5_* exports), so older builds
        of our own proxies still classify as ours and get cleaned.
        dxgi.dll exists in two modes (DXVK's for dx11, m12's for dx12) -
        the marker decides which mode it belongs to."""
        tgt = self._target(dll_name)
        if not os.path.exists(tgt):
            return False
        if (os.path.exists(source_path)
                and _sha1(tgt) == _sha1(source_path)):
            return True
        try:
            with open(tgt, "rb") as f:
                data = f.read()
        except OSError:
            return False
        if mode == "dx12" and dll_name == "dxgi.dll":
            return b"m12_dxgi" in data
        if mode == "dx12" and dll_name == "d5c_chain.dll":
            return b"dlss5_create" in data
        if mode in ("dx9", "dx11"):
            return b"DXVK" in data
        return False

    def _is_ours_any_mode(self, dll_name, source_path):
        """True when the target is any known build of our own proxy,
        regardless of which mode produced it (marker-based)."""
        if (os.path.exists(source_path)
                and _sha1(self._target(dll_name)) == _sha1(source_path)):
            return True
        for mode in MODE_DLLS:
            if self._is_ours(dll_name, mode, source_path):
                return True
        return False


class DeployNeedsElevation(DeployError):
    """The game dir is ACL-protected (Program Files, launcher-owned)."""


# --------------------------------------------------------- elevated copy ---
_PS_COPY = r"""
param([string]$JsonFile)
$pairs = (Get-Content -Raw $JsonFile) | ConvertFrom-Json
foreach ($p in $pairs) {
    $dst = $p.dst; $bak = $dst + '.m13bak'
    if ((Test-Path $dst) -and -not (Test-Path $bak)) {
        Copy-Item $dst $bak -Force
    }
    Copy-Item $p.src $dst -Force
}
"""

_PS_REMOVE = r"""
param([string]$JsonFile)
$Dst = (Get-Content -Raw $JsonFile) | ConvertFrom-Json
$bak = $Dst + '.m13bak'
if (Test-Path $Dst) { Remove-Item $Dst -Force }
if (Test-Path $bak) { Move-Item $bak $Dst -Force }
"""


def _run_elevated(ps_body, arg):
    """Run a small PowerShell script elevated (one UAC prompt), wait for it.
    Quoting gotcha: the script goes to a temp .ps1, the arg is a JSON file.
    -Command nested inside -ArgumentList mangled the inner double quotes on
    this host (elevated steps silently no-oped); -File + -JsonFile keeps every
    argument a verbatim single-quoted element instead."""
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
    cmd = ["powershell", "-NoProfile", "-Command",
           "Start-Process", "powershell", "-Verb", "RunAs", "-Wait",
           "-WindowStyle", "Hidden", "-ArgumentList",
           "'-NoProfile','-ExecutionPolicy','Bypass','-File','\"%s\"',"
           "'-JsonFile','\"%s\"'" % (script, argfile)]
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
    Returns a list of human warnings ([] = clear to deploy).

    Detection: the PE header does NOT carry a 'DXVK' literal in its first
    bytes (a previous version checked head[:64] and could never fire); the
    project name lives in version info, so scan the whole file. A foreign
    non-DXVK dxgi.dll (ReShade & co.) is the owner's choice - it warns only
    in launch logs, it does not block. Unreadable file blocks: unknown
    provenance at a known-crash site is not worth the dice roll."""
    warnings = []
    dxgi = os.path.join(game_dir, "dxgi.dll")
    if os.path.exists(dxgi):
        try:
            with open(dxgi, "rb") as f:
                blob = f.read()
            # DXVK builds name themselves in version info; the .gta4.bak
            # sidecar proves this crash happened once already.
            if b"DXVK" in blob or os.path.exists(dxgi + ".gta4.bak"):
                warnings.append("dxgi.dll in the game dir is DXVK - "
                                "GTA IV crashed on that mix (system d3d11 + "
                                "DXVK dxgi). Remove it or the game may die.")
        except OSError:
            warnings.append("dxgi.dll exists in the game dir - not touched, "
                            "but for D3D9 games only d3d9.dll may come from "
                            "DXVK.")
    return warnings


def dx9_quarantine(game_dir):
    """Move a proven-poison DXVK dxgi.dll aside (reversible .bak rename).
    Returns (moved, detail). Only call after dx9_guard flagged it - foreign
    non-DXVK injectors are never touched."""
    dxgi = os.path.join(game_dir, "dxgi.dll")
    if not os.path.exists(dxgi):
        return False, "nothing to quarantine"
    bak = dxgi + ".dlssnr-poison.bak"
    try:
        if os.path.exists(bak):
            os.remove(bak)
        os.replace(dxgi, bak)
        return True, bak
    except OSError as e:
        return False, str(e)
