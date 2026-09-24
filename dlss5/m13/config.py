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
    "weights_path": None,        # asked at first run (file dialog)
    "gain": 1.0,                 # last used gain, pushed to m11d live
    "overlay_hotkey": "control+alt+g",   # show/hide the gain knob
    "dx9_game": None,            # absolute path to the DX9 game exe
    "dx12_game": None,           # absolute path to the DX12 game exe
    "screen_args": "",           # extra m8blive args for screen mode
}


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

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="ascii") as f:
            json.dump(self.data, f, indent=2, sort_keys=True)
        os.replace(tmp, self.path)   # atomic-ish: no half-written config

    def get(self, key):
        return self.data.get(key)

    def set(self, key, value):
        self.data[key] = value
        self.save()

    @property
    def first_run(self):
        return not self.data.get("weights_path")
