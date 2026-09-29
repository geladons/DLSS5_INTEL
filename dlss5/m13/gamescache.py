# ============================================================================
# m13.gamescache - persistent cache for scanned games list.
#
# Storage: %LOCALAPPDATA%\DLSS5Manager\games_cache.json
# Loading the cached games list on startup allows the UI to display cards
# immediately with 0 ms drive-scan delay. Users can trigger a manual rescan
# at any time via the "Rescan Games" button.
# ============================================================================
import json
import os

try:
    from .config import CONFIG_DIR
    from .gamescan import Game
except (ImportError, ValueError):
    from config import CONFIG_DIR
    from gamescan import Game


GAMES_CACHE_PATH = os.path.join(CONFIG_DIR, "games_cache.json")


def load_cache(path=None):
    """Load cached games from disk -> list[Game]. Returns [] on miss/error."""
    path = path or GAMES_CACHE_PATH
    if not os.path.isfile(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, list):
            return []
        games = []
        for item in data:
            if not isinstance(item, dict):
                continue
            exe = item.get("exe")
            if not exe or not os.path.exists(exe):
                continue
            source = item.get("source", "cached")
            root = item.get("root")
            name = item.get("name")
            g = Game(exe, source, root=root, name=name)
            g.launch_exe = item.get("launch_exe") or exe
            g.arch = item.get("arch")
            g.apis = item.get("apis") or []
            g.size = item.get("size") or 0
            g.likely = bool(item.get("likely", True))
            g.anticheat = bool(item.get("anticheat", False))
            games.append(g)
        return games
    except (OSError, ValueError):
        return []


def save_cache(games, path=None):
    """Serialize list[Game] to atomic JSON cache."""
    path = path or GAMES_CACHE_PATH
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        entries = []
        for g in games:
            entries.append({
                "exe": g.exe,
                "launch_exe": g.launch_exe,
                "root": g.root,
                "name": g.name,
                "source": g.source,
                "arch": g.arch,
                "apis": g.apis,
                "size": g.size,
                "likely": g.likely,
                "anticheat": g.anticheat,
            })
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(entries, f, indent=2, ensure_ascii=False)
        os.replace(tmp, path)
        return True
    except (OSError, ValueError):
        return False


def clear_cache(path=None):
    """Remove cache file if present."""
    path = path or GAMES_CACHE_PATH
    try:
        if os.path.exists(path):
            os.remove(path)
    except OSError:
        pass
