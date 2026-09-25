# ============================================================================
# m13.gamescan - find REAL installed games (not every exe on the drive) and
# pick the right injection mode.
#
# v2 (owner feedback: "180 games found, really I have 10"):
#   - candidates are GROUPED per install root (Steam common\<game>, top-level
#     folder on a data drive, one app folder under Program Files); a group
#     becomes a game ONLY when it contains at least one exe with graphics-API
#     evidence (static imports, delay-loads or dll-name strings). One group =
#     one game card. Tools/launchers/updaters never become cards.
#   - LAUNCHER WRAPPERS: pirate/GOG/Rockstar installs often start through a
#     tiny stub (D:\STALKER2\Stalker2.exe, PlayRDR2.exe, Launcher.exe) while
#     the real renderer binary sits deeper (Stalker2\Binaries\Win64\...
#     Shipping.exe). The card's inject target is the REAL binary (mode detect,
#     DLL deploy); launch_exe is the wrapper when one is detected, so the
#     crack/launcher chain still runs.
#   - pe_info() now also parses the DELAY-LOAD import directory: UE5 titles
#     (STALKER 2) delay-load d3d12.dll, so the classic import table alone
#     mis-detects them as dx11 (they statically import d3d11 for the RHI but
#     render through d3d12).
#
# pe_info() is a tiny stdlib-only PE reader: bitness from the optional-header
# magic, graphics API from the import + delay-import tables and dll-name
# strings (mmap: game exes are tens of MB, the tables can sit far in).
# Suggested mode: vulkan > dx12 > dx11 > dx9 (vulkan needs NO dll deploy -
# the registered implicit layer loads by itself). Known anti-cheat titles get
# a warning flag - injecting a DLL into those can get the owner BANNED online.
# ============================================================================
import os
import re

# Junk exe names: never a game binary and never a launch wrapper.
JUNK_WORDS = ("unins", "setup", "redist", "crash", "report", "updater",
              "helper", "service", "broker", "cef", "eac", "battleye",
              "_be", "be.exe", "anticheat", "installer", "vcred", "dotnet",
              "dxsetup", "maintenancetool", "torrent", "vkconfig", "sunshine",
              "obs64", "sharex", "notepad", "websetup", "patcher",
              "protected", "fossilize", "cmake", "dxgi-info", "vkcube",
              "msedge", "chrome", "firefox", "opera", "brave", "iexplore",
              "onedrive", "teams", "spotify", "discord", "steamwebhelper",
              "copilot")

# Launcher-ish names: fine as a launch wrapper, never the renderer binary.
WRAPPER_HINTS = ("launcher", "launch", "play", "start", "run")
MAIN_BAD = ("launcher", "patcher")

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

# Directories never descended into while scanning a group.
DIR_SKIP = ("redist", "redistributables", "_commonredist", "_redist",
            "support", "installers", "directx", "dotnet", "winsxs",
            "appdata", "documents", "saved games", "screenshots", "mods",
            "uninstall", "language changer", "crashdumps", "logs")

WRAPPER_MAX_SIZE = 15 * 1024 * 1024     # a wrapper stub is small
MAIN_MIN_SIZE = 300 * 1024              # a real renderer binary is not tiny


class Game:
    def __init__(self, exe, source, root=None, name=None):
        self.exe = exe                # inject target (mode detect + deploy)
        self.launch_exe = exe         # what "Launch" actually starts
        self.root = root or os.path.dirname(exe)
        self.name = name or os.path.splitext(os.path.basename(exe))[0]
        self.source = source
        self.arch = None              # "x64" | "x86" | None (unreadable)
        self.apis = []
        self.size = 0
        self.likely = False           # chosen as the group's main binary
        self.anticheat = any(h in exe.lower() for h in ANTICHEAT_HINTS)

    @property
    def mode(self):
        for m in API_PRIORITY:
            if m in self.apis:
                return m
        return None

    @property
    def via_wrapper(self):
        return self.launch_exe.lower() != self.exe.lower()

    def __repr__(self):
        return "Game(%s %s %s%s)" % (
            self.name, self.arch, self.apis,
            " via %s" % os.path.basename(self.launch_exe)
            if self.via_wrapper else "")


# ------------------------------------------------------------ PE parsing ---
def _read_imports(data, rva2off, imp_rva, stride, name_off, bound=4096):
    """Walk an import-style directory; returns a set of dll names (lower)."""
    dlls = set()
    desc = rva2off(imp_rva)
    for _ in range(bound):
        if desc is None or desc + stride > len(data):
            break
        name_rva = int.from_bytes(data[desc + name_off:desc + name_off + 4],
                                  "little")
        if name_rva == 0:
            break
        noff = rva2off(name_rva & 0x7FFFFFFF)   # delay-load names may be RVA
        if noff is not None and noff < len(data):
            z = data.find(b"\0", noff, noff + 64)
            if z > noff:
                dlls.add(data[noff:z].decode("ascii", "replace").lower())
        desc += stride
    return dlls


def pe_info(exe):
    """-> (arch, apis). Pure-Python PE import walk; never raises.
    Covers the classic import table AND the delay-load directory (UE5 games
    like STALKER 2 delay-load d3d12.dll - the plain table says d3d11)."""
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

            # classic import table (data directory #1)
            imp_rva = int.from_bytes(data[dd_base + 8:dd_base + 12], "little")
            imp_sz = int.from_bytes(data[dd_base + 12:dd_base + 16], "little")
            if imp_rva and imp_sz:
                dlls |= _read_imports(data, rva2off, imp_rva, 20, 12)
            # delay-load directory (data directory #13): 32-byte entries,
            # name RVA at offset 4 (UE5 d3d12.dll lives HERE)
            dly_rva = int.from_bytes(data[dd_base + 13 * 8:dd_base + 13 * 8 + 4],
                                     "little")
            if dly_rva:
                dlls |= _read_imports(data, rva2off, dly_rva, 32, 4, bound=256)
            # Dynamic loading: games like GTA5 Enhanced LoadLibrary their
            # D3D runtime, so imports alone miss it - scan for the dll name
            # strings in the whole binary (mmap find is C-speed). Metro
            # Exodus keeps the names as UTF-16 - scan wide strings too, but
            # NOT for vulkan-1.dll: STALKER 2 carries a wide vulkan string
            # (bundled XeSS) while being a pure DX12 title.
            for needle in (b"d3d12.dll", b"d3d11.dll", b"d3d10core.dll",
                           b"d3d10.dll", b"d3d9.dll", b"vulkan-1.dll"):
                if data.find(needle) >= 0 or \
                        data.find(needle.upper()) >= 0:
                    dyn_dlls.add(needle.decode())
            for needle in (b"d3d12.dll", b"d3d11.dll", b"d3d10core.dll",
                           b"d3d10.dll", b"d3d9.dll"):
                wide = bytearray()
                for c in needle:
                    wide += bytes((c, 0))
                if data.find(bytes(wide)) >= 0:
                    dyn_dlls.add(needle.decode())
        finally:
            mm.close()
    except (OSError, IndexError, ValueError):
        return arch, []
    # Classification rules (learned on the owner's library):
    #  1. vulkan-1.dll anywhere -> Vulkan: needs NO dll deploy (the implicit
    #     layer loads by itself). RDR2 statically imports d3d9.dll yet is a
    #     Vulkan/DX12 game - the dll names alone lie.
    #  2. else trust STATIC + DELAY-LOAD imports (precise PE directories)
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
             "users", "pagefile.sys", "amd", "nvidia", "vulkansdk",
             "virtualdisplaydriver", "git", "inetpub", "temp",
             "windowsapps", "wpsystem", "msdownld.tmp",
             "wpmod", "xboxgames")
# Program Files: scanned one app-folder at a time (each folder = a group).
APPSTORE_DIRS = ("program files", "program files (x86)")
# Container folders whose children are covered by launcher manifests.
STORE_CONTAINERS = ("steam", "common files", "epic games", "gog galaxy")


def steam_libraries():
    """All Steam library roots from libraryfolders.vdf. The install path
    comes from the registry (HKCU\\Software\\Valve\\Steam); the default
    location is the fallback."""
    roots = []
    base = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Valve\Steam") as k:
            base = winreg.QueryValueEx(k, "SteamPath")[0]
    except (ImportError, OSError):
        pass
    if not base or not os.path.isdir(base):
        base = r"C:\Program Files (x86)\Steam"
    vdf = os.path.join(base, "steamapps", "libraryfolders.vdf")
    try:
        with open(vdf, "r", errors="replace") as f:
            for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                roots.append(m.group(1).replace("\\\\", "\\"))
    except OSError:
        pass
    if not roots and os.path.isdir(os.path.join(base, "steamapps")):
        roots.append(base)
    return [r for r in roots if os.path.isdir(r)]


def steam_games(lib):
    """-> [(name, installdir)] from appmanifest_*.acf of one library."""
    out = []
    apps = os.path.join(lib, "steamapps")
    try:
        names = [n for n in os.listdir(apps)
                 if n.startswith("appmanifest_") and n.endswith(".acf")]
    except OSError:
        return out
    for n in names:
        try:
            with open(os.path.join(apps, n), "r", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        name = re.search(r'"name"\s+"([^"]+)"', text)
        inst = re.search(r'"installdir"\s+"([^"]+)"', text)
        if inst:
            out.append((name.group(1) if name else None, inst.group(1)))
    return out


def epic_games():
    """-> [(name, install_dir)] from Epic launcher JSON manifests."""
    import glob
    import json
    out = []
    man = os.path.join(os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
                       "Epic", "EpicGamesLauncher", "Data", "Manifests")
    for f in glob.glob(os.path.join(man, "*.item")):
        try:
            with open(f, "r", errors="replace") as fh:
                j = json.load(fh)
        except (OSError, ValueError):
            continue
        loc = j.get("InstallLocation")
        if loc and os.path.isdir(loc):
            out.append((j.get("DisplayName"), loc))
    return out


def gog_games():
    """-> [(name, install_dir)] from GOG Galaxy registry entries."""
    out = []
    try:
        import winreg
    except ImportError:
        return out
    key_path = r"SOFTWARE\WOW6432Node\GOG.com\Games"
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            with winreg.OpenKey(hive, key_path) as k:
                i = 0
                while True:
                    try:
                        sub = winreg.EnumKey(k, i)
                    except OSError:
                        break
                    i += 1
                    try:
                        with winreg.OpenKey(k, sub) as sk:
                            path = winreg.QueryValueEx(sk, "path")[0]
                            try:
                                name = winreg.QueryValueEx(sk, "gameName")[0]
                            except OSError:
                                name = None
                    except OSError:
                        continue
                    if path and os.path.isdir(path):
                        out.append((name, path))
        except OSError:
            continue
    return out


def _collect_exes(root, depth):
    """All *.exe under root up to depth levels deep (junk dirs skipped)."""
    out = []
    if depth < 0 or not os.path.isdir(root):
        return out
    try:
        entries = list(os.scandir(root))
    except OSError:
        return out
    for e in entries:
        if e.is_file() and e.name.lower().endswith(".exe"):
            out.append(e.path)
    if depth:
        for e in entries:
            if e.is_dir() and e.name.lower() not in DIR_SKIP:
                out.extend(_collect_exes(e.path, depth - 1))
    return out


def _norm(s):
    return re.sub(r"[^a-z0-9]+", "", s.lower())


def _is_junk_name(exe):
    low = os.path.basename(exe).lower()
    return any(w in low for w in JUNK_WORDS)


def _is_main_candidate(exe):
    """A renderer binary: not junk and not a launcher/patcher stub."""
    low = os.path.basename(exe).lower()
    return not _is_junk_name(exe) and not any(w in low for w in MAIN_BAD)


def _pick_wrapper(root_exes, main_exe, folder_name):
    """Choose the launch wrapper among the group's ROOT-level exes, or None.
    A wrapper is small, not junk, and either launcher-named or named like the
    game folder / the main binary (GOG stub Stalker2.exe, PlayRDR2.exe)."""
    main_stem = _norm(os.path.splitext(os.path.basename(main_exe))[0])
    folder_norm = _norm(folder_name)
    best, best_rank = None, -1
    for exe in root_exes:
        if exe.lower() == main_exe.lower() or _is_junk_name(exe):
            continue
        try:
            size = os.path.getsize(exe)
        except OSError:
            continue
        if size > WRAPPER_MAX_SIZE:
            continue
        stem = _norm(os.path.splitext(os.path.basename(exe))[0])
        rank = -1
        if "launcher" in stem:
            rank = 3
        elif any(stem.startswith(h) for h in ("play", "start", "run", "launch")):
            rank = 2
        elif stem and (stem == folder_norm
                       or (len(stem) >= 4 and main_stem.startswith(stem))
                       or (len(stem) >= 4 and stem in main_stem)):
            rank = 1
        if rank > best_rank:
            best, best_rank = exe, rank
    return best


def _game_from_group(root, source, name=None, depth=4):
    """One install root -> one Game, or None when nothing renderable lives
    inside (tools, launchers, redists, random app folders)."""
    exes = _collect_exes(root, depth)
    if not exes:
        return None
    root_level = [e for e in exes
                  if os.path.dirname(e).lower() == os.path.abspath(root).lower()]
    mains = []
    for e in exes:
        if not _is_main_candidate(e):
            continue
        try:
            size = os.path.getsize(e)
        except OSError:
            continue
        if size < MAIN_MIN_SIZE:
            continue
        arch, apis = pe_info(e)
        if not apis:
            continue                      # no graphics API -> not a game binary
        g = Game(e, source, root=root, name=name)
        g.arch, g.apis, g.size = arch, apis, size
        mains.append(g)
    if not mains:
        return None
    # main binary: biggest API-positive exe; prefer a name similar to the
    # folder on ties (avoids picking a bundled dedicated server etc.)
    folder_norm = _norm(os.path.basename(os.path.abspath(root)))
    mains.sort(key=lambda g: (
        -g.size,
        0 if _norm(os.path.splitext(os.path.basename(g.exe))[0]) in folder_norm
        or folder_norm in _norm(os.path.splitext(os.path.basename(g.exe))[0])
        else 1))
    main = mains[0]
    main.likely = True
    if not name:
        # a folder name reads far better than "Stalker2-Win64-Shipping"
        name = os.path.basename(os.path.abspath(root))
        main.name = name
    wrapper = _pick_wrapper(root_level, main.exe,
                            os.path.basename(os.path.abspath(root)))
    if wrapper:
        main.launch_exe = wrapper
    return main


def scan(extra_dirs=()):
    """-> [Game]: one entry per REAL game found on this machine.
    Sources: Steam manifests (proper names), Epic manifests, GOG registry,
    then every fixed drive grouped per install root. Groups without an
    API-positive binary are dropped - that is how 180 exe candidates become
    the actual ~10 games."""
    games = {}
    steam_roots = []
    for lib in steam_libraries():
        common = os.path.join(lib, "steamapps", "common")
        steam_roots.append(os.path.abspath(common).lower())
        for name, installdir in steam_games(lib):
            g = _game_from_group(os.path.join(common, installdir), "steam",
                                 name=name)
            if g:
                games[g.exe.lower()] = g
    store_roots = []
    for source, entries in (("epic", epic_games()), ("gog", gog_games())):
        for name, path in entries:
            store_roots.append(os.path.abspath(path).lower())
            g = _game_from_group(path, source, name=name)
            if g and g.exe.lower() not in games:
                games[g.exe.lower()] = g
    covered = steam_roots + store_roots
    for drive in fixed_drives():
        try:
            top = [e for e in os.scandir(drive) if e.is_dir()]
        except OSError:
            continue
        for e in top:
            name = e.name.lower()
            if name in ROOT_SKIP:
                continue
            if any(os.path.abspath(e.path).lower().startswith(r)
                   for r in covered):
                continue                  # store manifests cover their own
            if name in APPSTORE_DIRS:
                try:
                    subs = [s for s in os.scandir(e.path) if s.is_dir()]
                except OSError:
                    continue
                for s in subs:
                    if s.name.lower() in STORE_CONTAINERS:
                        continue          # stores have their own manifests
                    g = _game_from_group(s.path, e.name, depth=3)
                    if g and g.exe.lower() not in games:
                        games[g.exe.lower()] = g
                continue
            # A folder with NO root-level exes is a container (downloads,
            # "Games"): each subfolder is its own install root.
            try:
                has_root_exe = any(x.is_file()
                                   and x.name.lower().endswith(".exe")
                                   for x in os.scandir(e.path))
            except OSError:
                has_root_exe = False
            if not has_root_exe:
                try:
                    subs = [s for s in os.scandir(e.path) if s.is_dir()]
                except OSError:
                    continue
                for s in subs:
                    g = _game_from_group(s.path, "drive " + drive[0],
                                         depth=3)
                    if g and g.exe.lower() not in games:
                        games[g.exe.lower()] = g
                continue
            g = _game_from_group(e.path, "drive " + drive[0])
            if g and g.exe.lower() not in games:
                games[g.exe.lower()] = g
    for d in extra_dirs:
        g = _game_from_group(d, "manual")
        if g and g.exe.lower() not in games:
            games[g.exe.lower()] = g
    out = sorted(games.values(), key=lambda g: g.name.lower())
    return out
