# ============================================================================
# m13.gamescan - find installed games and pick the right injection mode.
#
# Scanner sources (no registry writes, read-only):
#   - Steam: every library from steamapps\libraryfolders.vdf -> <lib>\steamapps
#     \common\<game>\**\*.exe (max depth 3 below common\<game>)
#   - Rockstar / Epic / GOG / plain roots (shallow scan)
#   - manual entries from the manager
# Junk exes (uninstallers, launchers, crash reporters, redists) are filtered;
# the largest exe in a folder is flagged as the likely game binary.
#
# pe_info() is a tiny stdlib-only PE reader: bitness from the optional-header
# magic, graphics API from the import table (d3d9/d3d10/d3d11/d3d12/dxgi).
# Suggested mode: dx12 > dx11 > dx9. Known anti-cheat titles get a warning
# flag - injecting a DLL into those can get the owner BANNED online.
# ============================================================================
import os
import re

JUNK_WORDS = ("unins", "setup", "redist", "crash", "report", "updater",
              "launcher", "helper", "service", "broker", "cef", "eac",
              "battleye", "_be", "be.exe", "anticheat", "installer",
              "vcred", "dotnet", "dxsetup", "maintenancetool", "torrent",
              "vkconfig", "sunshine", "obs64", "sharex", "notepad")

ANTICHEAT_HINTS = ("pubg", "counter-strike", "cs2", "valorant", "fortnite",
                   "apex", "destiny", "rust", "rainbow", "tarkov", "faceit",
                   "throne", "gta5_enhanced_be", "playgtav")

# Priority order = detect order: a Vulkan-native game needs NO dll at all
# (the registered implicit layer loads by itself), so it wins over d3d
# strings; d3d9 strings sit in many multi-renderer binaries (RDR2) so dx9
# is last.
API_PRIORITY = ("vulkan", "dx12", "dx11", "dx9")
API_DLLS = {"vulkan": ("vulkan-1.dll",), "dx12": ("d3d12.dll",),
            "dx11": ("d3d11.dll", "d3d10.dll", "d3d10_1.dll",
                     "d3d10core.dll"), "dx9": ("d3d9.dll",)}


class Game:
    def __init__(self, exe, source):
        self.exe = exe
        self.name = os.path.splitext(os.path.basename(exe))[0]
        self.source = source
        self.arch = None          # "x64" | "x86" | None (unreadable)
        self.apis = []            # subset of ("dx12","dx11","dx9")
        self.size = 0
        self.likely = False       # biggest exe in its folder
        self.anticheat = any(h in exe.lower() for h in ANTICHEAT_HINTS)

    @property
    def mode(self):
        for m in API_PRIORITY:
            if m in self.apis:
                return m
        return None

    def __repr__(self):
        return "Game(%s %s %s)" % (self.name, self.arch, self.apis)


# ------------------------------------------------------------ PE parsing ---
def pe_info(exe):
    """-> (arch, apis). Pure-Python PE import walk; never raises.
    Uses mmap: game exes are tens of MB, the import directory can sit far
    past the first few MB (that is why a plain 4 MB head read missed it)."""
    import mmap
    arch, dlls, dyn_dlls = None, set(), set()
    try:
        with open(exe, "rb") as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            data = mm
            if data[:2] != b"MZ":
                return None, []
            peoff = int.from_bytes(data[0x3C:0x40], "little")
            if data[peoff:peoff + 4] != b"PE\0\0":
                return None, []
            nsec = int.from_bytes(data[peoff + 6:peoff + 8], "little")
            optsz = int.from_bytes(data[peoff + 20:peoff + 22], "little")
            opt = peoff + 24
            magic = int.from_bytes(data[opt:opt + 2], "little")
            if magic == 0x20B:
                arch = "x64"
                dd_base = opt + 112
            elif magic == 0x10B:
                arch = "x86"
                dd_base = opt + 96
            else:
                return None, []
            imp_rva = int.from_bytes(data[dd_base + 8:dd_base + 12], "little")
            imp_sz = int.from_bytes(data[dd_base + 12:dd_base + 16], "little")
            if not imp_rva or not imp_sz:
                return arch, []
            secs = []
            sbase = opt + optsz
            for i in range(nsec):
                s = sbase + i * 40
                va = int.from_bytes(data[s + 12:s + 16], "little")
                vsz = int.from_bytes(data[s + 8:s + 12], "little")
                raw = int.from_bytes(data[s + 20:s + 24], "little")
                secs.append((va, max(vsz, 1), raw))

            def rva2off(rva):
                for va, vsz, raw in secs:
                    if va <= rva < va + vsz:
                        return raw + (rva - va)
                return None

            desc = rva2off(imp_rva)
            for _ in range(4096):       # bounded walk, no infinite loops
                if desc is None or desc + 20 > len(data):
                    break
                name_rva = int.from_bytes(data[desc + 12:desc + 16], "little")
                if name_rva == 0:
                    break
                noff = rva2off(name_rva)
                if noff is not None and noff < len(data):
                    z = data.find(b"\0", noff, noff + 64)
                    if z > noff:
                        dlls.add(data[noff:z].decode("ascii",
                                                     "replace").lower())
                desc += 20
            # Dynamic loading: games like GTA5 Enhanced LoadLibrary their
            # D3D runtime, so imports alone miss it - scan for the dll name
            # strings in the whole binary (mmap find is C-speed).
            for needle in (b"d3d12.dll", b"d3d11.dll", b"d3d10core.dll",
                           b"d3d10.dll", b"d3d9.dll", b"vulkan-1.dll"):
                if data.find(needle) >= 0 or \
                        data.find(needle.upper()) >= 0:
                    dyn_dlls.add(needle.decode())
        finally:
            mm.close()
    except (OSError, IndexError, ValueError):
        return arch, []
    # Classification rules (learned on the owner's library):
    #  1. vulkan-1.dll anywhere -> Vulkan: needs NO dll deploy (the implicit
    #     layer loads by itself). RDR2 statically imports d3d9.dll yet is a
    #     Vulkan/DX12 game - the dll names alone lie.
    #  2. else trust STATIC imports (GTA IV CE carries a stray d3d10 string)
    #  3. else dynamic strings (GTA5 Enhanced LoadLibraries d3d12)
    if any(d in dlls or d in dyn_dlls for d in API_DLLS["vulkan"]):
        return arch, ["vulkan"]
    static = [m for m in API_PRIORITY if any(d in dlls for d in API_DLLS[m])]
    if static:
        return arch, static
    return arch, [m for m in API_PRIORITY
                  if any(d in dyn_dlls for d in API_DLLS[m])]


# --------------------------------------------------------------- scanning --
def fixed_drives():
    """All fixed-drive roots (C:\\, D:\\, ...) via GetDriveTypeW."""
    import ctypes
    roots = []
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for i in range(26):
        if mask & (1 << i):
            root = "%s:\\" % chr(65 + i)
            if ctypes.windll.kernel32.GetDriveTypeW(root) == 3:  # FIXED
                roots.append(root)
    return roots


# Never descend into these at drive-root level (system noise, not games).
ROOT_SKIP = ("windows", "$recycle.bin", "system volume information",
             "programdata", "recovery", "perflogs", "msocache", "intel",
             "users", "pagefile.sys", "amd", "nvidia")
# Inside these, games DO live - scan them, but not their system subdirs.
VENDOR_SKIP = ("windows", "microsoft", "common files", "internet explorer",
               "windowsapps", "windows defender", "dotnet", "msbuild")


def steam_libraries():
    """All Steam library roots from libraryfolders.vdf."""
    roots = []
    vdf = r"C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf"
    try:
        with open(vdf, "r", errors="replace") as f:
            for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                roots.append(m.group(1).replace("\\\\", "\\"))
    except OSError:
        pass
    return [r for r in roots if os.path.isdir(r)]


def _scan_dir(root, depth, out, source):
    """Collect *.exe under root up to depth levels deep."""
    if depth < 0 or not os.path.isdir(root):
        return
    try:
        entries = list(os.scandir(root))
    except OSError:
        return
    for e in entries:
        if e.is_file() and e.name.lower().endswith(".exe"):
            low = e.name.lower()
            if not any(w in low for w in JUNK_WORDS):
                out.append(Game(e.path, source))
    if depth:
        for e in entries:
            if e.is_dir() and not e.name.lower() in (
                    "redist", "redistributables", "_commonredist", "support",
                    "installers", "directx", "dotnet", "winsxs", "appdata",
                    "documents", "saved games", "screenshots", "mods"):
                _scan_dir(e.path, depth - 1, out, source)


def scan(extra_dirs=()):
    """-> [Game] sorted: likely games first, then by size desc.
    Covers every fixed drive (any install location), not just launchers."""
    games = []
    for lib in steam_libraries():
        common = os.path.join(lib, "steamapps", "common")
        try:
            subs = [e.path for e in os.scandir(common) if e.is_dir()]
        except OSError:
            subs = []
        for game_dir in subs:
            _scan_dir(game_dir, 3, games, "steam")
    for drive in fixed_drives():
        try:
            top = [e for e in os.scandir(drive) if e.is_dir()]
        except OSError:
            continue
        for e in top:
            name = e.name.lower()
            if name in ROOT_SKIP:
                continue
            # C:\ root dirs are mostly system - depth 2; data drives depth 3
            depth = 2 if drive.lower().startswith("c:") else 3
            _scan_dir(e.path, depth, games, "drive " + drive[0])
    for d in extra_dirs:
        _scan_dir(d, 3, games, "manual")

    seen, uniq = set(), []
    for g in games:
        if g.exe.lower() not in seen:
            seen.add(g.exe.lower())
            uniq.append(g)
    by_dir = {}
    for g in uniq:
        try:
            g.size = os.path.getsize(g.exe)
        except OSError:
            pass
        by_dir.setdefault(os.path.dirname(g.exe).lower(), []).append(g)
    for group in by_dir.values():
        big = max(group, key=lambda g: g.size)
        if big.size > 0:
            big.likely = True
    for g in uniq:
        g.arch, g.apis = pe_info(g.exe)
    uniq.sort(key=lambda g: (not g.likely, not g.mode, -g.size))
    return uniq
