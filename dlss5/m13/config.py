# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.config - persistent user configuration.
#
# The weights are PROPRIETARY (leaked) - we never ship them. On first run the
# app asks for the .safetensors path and remembers it here. Everything else
# (gain, games, hotkey) is ordinary user preference.
#
# Storage: %LOCALAPPDATA%\DLSS5Manager\config.json (user-writable, no admin).
# ============================================================================
import json
import os

CONFIG_DIR = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                          "DLSS5Manager")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")

DEFAULTS = {
    "weights_path": None,        # autodetected from the bundle, else asked
    "gain": 1.0,                 # legacy residual gain; neutral public baseline
    "intensity": 1.0,            # Phase30 compose mix; neutral public baseline
    "local_tone": 1.0,           # Phase30: local tone / brightness (0.0..2.0)
    "local_structure": 1.0,      # Phase30: local structure / detail strength (0.0..2.0)
    "skin_structure": -1.0,      # Phase30: optional skin condition disabled
    "auto_mask": False,          # Phase30: optional skin/UI condition disabled
    "style_index": 0,            # Phase30: style index (0 = Default, 1 = Natural, 2 = Cinematic)
    "color_strength": 1.0,       # Phase30: color component strength (0.0..2.0)
    "blend": 1.0,                # effect mix: 0 = original, 1 = full network
    "scale": 0.50,               # Preset options: 1.00 Native, 0.67 Quality, 0.58 Balanced, 0.50 Performance, 0.33 Ultra Performance
    "frame_gen": True,           # Frame Generation enabled by default
    "sharpness": 0.8,            # bounded CAS sharpness 0.0 to 1.0 (encode.comp)
    "language": "en",            # UI language: en (primary) | ru
    "overlay_hotkey": "control+alt+g",   # show/hide the in-game overlay
    "dx9_game": None,            # legacy single-game keys (v1 config)
    "dx12_game": None,
    "games": {},                 # exe path -> {"mode": dx9|dx11|dx12,
                                 #              "arch": x64|x86, "name": str,
                                 #              "launch_exe": optional wrapper,
                                 #              "phase30": optional per-game dict}
    "screen_args": "",           # extra m8blive args for screen mode
    "overlay_autopause": False,  # pause processing when the overlay opens
    "overlay_freeze": False,     # FREEZE the frame while the overlay is open (False = live game keeps running)
    "overlay_focus": False,      # overlay uses WS_EX_NOACTIVATE without stealing focus;
                                 # game never minimizes on overlay open
    # ---- media tab ---------------------------------------------------------
    "media_out_dir": None,       # fixed output dir for mode "always"; None =
                                 # alongside the source (subdir "dlss5")
    "media_save_mode": "subdir", # ask | always | subdir
    "media_video_preset": "mp4", # video output container (h264 mp4)
    "media_preview_max": 1024,   # max image dimension fed to the engine for
                                 # PREVIEWS (0 = full-res, slow); Save and
                                 # Process all always run at full resolution
}


def _dedupe_games(games):
    """Merge games-dict entries that collide after path normalization
    (os.path.normcase(os.path.normpath(p))) - duplicates landed because
    keys were raw path strings (D:/x vs D:\\x, different case).

    Collision rule: prefer the record with a non-empty launch_exe (the
    launcher wrapper is the "more data" case), else keep the FIRST.
    Keys are stored in normpath form - canonical separators but ORIGINAL
    case; a normcase'd key would silently miss every reader that looks
    games up by os.path.abspath-style paths.

    Pure function, no I/O. Returns (deduped_dict, dropped_keys)."""
    order = []                     # first-seen normalized keys
    best = {}                      # normkey -> [stored_key, meta]
    dropped = []
    for exe, meta in (games or {}).items():
        if not isinstance(meta, dict):
            meta = {}
        norm = os.path.normcase(os.path.normpath(exe))
        key = os.path.normpath(exe)
        cur = best.get(norm)
        if cur is None:
            order.append(norm)
            best[norm] = [key, meta]
            continue
        if not cur[1].get("launch_exe") and meta.get("launch_exe"):
            dropped.append(cur[0])          # later record wins
            best[norm] = [key, meta]
        else:
            dropped.append(key)             # keep the first
    out = {}
    for norm in order:
        key, meta = best[norm]
        out[key] = meta
    return out, dropped


class Config:
    """JSON-backed config; attribute access via cfg.get/set + save()."""

    def __init__(self, path=CONFIG_PATH):
        self.path = path
        self.data = dict(DEFAULTS)
        self.load()

    def load(self):
        try:
            with open(self.path, "r", encoding="ascii") as f:
                stored = json.load(f)
            if isinstance(stored, dict):
                self.data.update({k: v for k, v in stored.items()
                                  if k in DEFAULTS})
        except (OSError, ValueError):
            pass   # first run or corrupt file -> defaults
        games = self.data.get("games")
        if isinstance(games, dict):
            deduped, dropped = _dedupe_games(games)
            if dropped:
                print("[config] dropped %d duplicate game entrie(s): %s"
                      % (len(dropped), ", ".join(dropped)), flush=True)
            self.data["games"] = deduped
        self._write_runtime_paths()

    def _write_runtime_paths(self):
        # Only the real manager config refreshes the shared fallback file;
        # tests pointing Config at tmp paths must not touch LOCALAPPDATA.
        if os.path.abspath(self.path) != os.path.abspath(CONFIG_PATH):
            return
        try:
            from . import gamelaunch
            gamelaunch.write_runtime_paths(self)
        except Exception:
            pass

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="ascii") as f:
            json.dump(self.data, f, indent=2, sort_keys=True)
        os.replace(tmp, self.path)   # atomic-ish: no half-written config
        self._write_runtime_paths()

    def get(self, key, default=None):
        if key == "gain" and "gain" not in self.data and "intensity" in self.data:
            return self.data["intensity"]
        if key == "intensity" and "intensity" not in self.data and "gain" in self.data:
            return self.data["gain"]
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value
        if key == "gain":
            self.data["intensity"] = float(value)
        elif key == "intensity":
            self.data["gain"] = float(value)
        self.save()

    def get_phase30(self, exe=None):
        """Retrieve Phase 30 settings for a specific game or globally."""
        out = {
            "intensity": float(self.get("intensity", 1.0)),
            "local_tone": float(self.get("local_tone", 1.0)),
            "local_structure": float(self.get("local_structure", 1.0)),
            "skin_structure": float(self.get("skin_structure", -1.0)),
            "auto_mask": bool(self.get("auto_mask", False)),
            "style_index": int(self.get("style_index", 0)),
            "color_strength": float(self.get("color_strength", 1.0)),
        }
        if exe:
            games = self.data.get("games") or {}
            gmeta = games.get(exe) or {}
            p_game = gmeta.get("phase30") or {}
            for k in out:
                if k in p_game:
                    out[k] = p_game[k]
        return out

    def set_phase30(self, params, exe=None):
        """Update Phase 30 settings globally and optionally per-game."""
        if exe:
            games = dict(self.data.get("games") or {})
            if exe in games:
                gmeta = dict(games[exe])
                p_game = dict(gmeta.get("phase30") or {})
                p_game.update(params)
                gmeta["phase30"] = p_game
                games[exe] = gmeta
                self.data["games"] = games
        else:
            for k, v in params.items():
                if k in ("intensity", "gain"):
                    self.data["intensity"] = float(v)
                    self.data["gain"] = float(v)
                elif k in self.data:
                    self.data[k] = v
        self.save()

    def reset_phase30(self, exe=None):
        """Reset Phase 30 settings to defaults."""
        try:
            from .phase30 import PHASE30_DEFAULTS, PHASE30_VENDOR_KEYS
        except (ImportError, ValueError):
            from phase30 import PHASE30_DEFAULTS, PHASE30_VENDOR_KEYS
        # Vendor knobs only: PHASE30_DEFAULTS now also lists the engine
        # knobs carried in the live knob file, but a Phase30 reset must not
        # touch scale/blend/sharpness/frame_gen.
        self.set_phase30({k: PHASE30_DEFAULTS[k] for k in PHASE30_VENDOR_KEYS},
                         exe=exe)


    @property
    def first_run(self):
        return not self.data.get("weights_path")
