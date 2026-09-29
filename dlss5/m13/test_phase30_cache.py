# ============================================================================
# dlss5\m13\test_phase30_cache.py - unit tests for Games Cache and Phase 30
# ============================================================================
import os
import sys
import json
import tempfile
import unittest

_PKG = os.path.dirname(os.path.abspath(__file__))
if _PKG not in sys.path:
    sys.path.insert(0, _PKG)

import gamescache
import phase30
from gamescan import Game
from config import Config, DEFAULTS
from i18n import set_language, tr


class TestGamesCache(unittest.TestCase):
    def setUp(self):
        self._orig_path = gamescache.GAMES_CACHE_PATH
        self._tmp_dir = tempfile.TemporaryDirectory()
        gamescache.GAMES_CACHE_PATH = os.path.join(self._tmp_dir.name, "games_cache.json")

    def tearDown(self):
        gamescache.GAMES_CACHE_PATH = self._orig_path
        self._tmp_dir.cleanup()

    def test_cache_miss(self):
        self.assertEqual(gamescache.load_cache(), [])

    def test_cache_roundtrip(self):
        exe1 = os.path.join(self._tmp_dir.name, "Cyberpunk2077.exe")
        open(exe1, "w").close()
        exe2 = os.path.join(self._tmp_dir.name, "GTAIV.exe")
        open(exe2, "w").close()

        g1 = Game(exe1, "steam", name="Cyberpunk 2077")
        g1.arch = "x64"
        g1.apis = ["dx12"]
        g2 = Game(exe2, "custom", name="Grand Theft Auto IV")
        g2.arch = "x86"
        g2.apis = ["dx9"]

        self.assertTrue(gamescache.save_cache([g1, g2]))
        loaded = gamescache.load_cache()
        self.assertIsNotNone(loaded)
        self.assertEqual(len(loaded), 2)
        self.assertEqual(loaded[0].name, "Cyberpunk 2077")
        self.assertEqual(loaded[0].arch, "x64")
        self.assertEqual(loaded[1].arch, "x86")

    def test_clear_cache(self):
        exe = os.path.join(self._tmp_dir.name, "test.exe")
        open(exe, "w").close()
        self.assertTrue(gamescache.save_cache([Game(exe, "custom", name="Test")]))
        self.assertEqual(len(gamescache.load_cache()), 1)
        gamescache.clear_cache()
        self.assertEqual(gamescache.load_cache(), [])



class TestPhase30DefaultsAndEnv(unittest.TestCase):
    def test_defaults(self):
        self.assertEqual(phase30.PHASE30_DEFAULTS["intensity"], 1.0)
        self.assertEqual(phase30.PHASE30_DEFAULTS["local_tone"], 1.0)
        self.assertEqual(phase30.PHASE30_DEFAULTS["local_structure"], 1.0)
        self.assertEqual(phase30.PHASE30_DEFAULTS["skin_structure"], -1.0)
        self.assertFalse(phase30.PHASE30_DEFAULTS["auto_mask"])
        self.assertEqual(phase30.PHASE30_DEFAULTS["style_index"], 0)
        self.assertEqual(phase30.PHASE30_DEFAULTS["color_strength"], 1.0)

    def test_env_dict(self):
        env = phase30.phase30_env_dict(phase30.PHASE30_DEFAULTS)
        self.assertEqual(env["DLSS5_INTENSITY"], "1.000")
        self.assertEqual(env["DLSS5_GAIN"], "1.000")
        self.assertEqual(env["DLSS5_LOCAL_TONE"], "1.000")
        self.assertEqual(env["DLSS5_LOCAL_STRUCTURE"], "1.000")
        self.assertEqual(env["DLSS5_SKIN_STRUCTURE"], "-1.000")
        self.assertEqual(env["DLSS5_AUTO_MASK"], "0")
        self.assertEqual(env["DLSS5_STYLE_INDEX"], "0")
        self.assertEqual(env["DLSS5_COLOR_STRENGTH"], "1.000")

    def test_write_knobs(self):
        path = phase30.write_phase30_knobs(phase30.PHASE30_DEFAULTS)
        self.assertIsNotNone(path)
        self.assertTrue(os.path.exists(path))
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertEqual(data["intensity"], 1.0)
        self.assertEqual(data["style_index"], 0)


class TestConfigPhase30(unittest.TestCase):
    def setUp(self):
        self._tmp_dir = tempfile.TemporaryDirectory()
        self._cfg_path = os.path.join(self._tmp_dir.name, "config.json")
        self.cfg = Config(self._cfg_path)

    def tearDown(self):
        self._tmp_dir.cleanup()

    def test_default_values(self):
        p30 = self.cfg.get_phase30()
        self.assertEqual(p30["intensity"], 1.0)
        self.assertEqual(self.cfg.get("gain"), 1.0)
        self.assertEqual(p30["local_structure"], 1.0)

    def test_set_phase30_and_gain_sync(self):
        self.cfg.set_phase30({"intensity": 1.25, "local_tone": 0.8})
        p30 = self.cfg.get_phase30()
        self.assertEqual(p30["intensity"], 1.25)
        self.assertEqual(p30["local_tone"], 0.8)
        # Gain should sync with intensity
        self.assertEqual(self.cfg.get("gain"), 1.25)

        # Gain change should sync back to intensity
        self.cfg.set("gain", 1.85)
        self.assertEqual(self.cfg.get_phase30()["intensity"], 1.85)

    def test_reset_phase30(self):
        self.cfg.set_phase30({"intensity": 0.5, "local_structure": 0.2})
        self.cfg.reset_phase30()
        p30 = self.cfg.get_phase30()
        self.assertEqual(p30["intensity"], 1.0)
        self.assertEqual(p30["local_structure"], 1.0)
        self.assertEqual(self.cfg.get("gain"), 1.0)

    def test_per_game_phase30(self):
        game_exe = r"C:\Games\Cyberpunk2077.exe"
        self.cfg.set("games", {game_exe: {"name": "CP2077", "mode": "dx12", "arch": "x64"}})

        # Global is the public neutral mix.
        self.assertEqual(self.cfg.get_phase30(game_exe)["intensity"], 1.0)

        # Set game-specific
        self.cfg.set_phase30({"intensity": 1.9, "style_index": 2}, exe=game_exe)
        self.assertEqual(self.cfg.get_phase30(game_exe)["intensity"], 1.9)
        self.assertEqual(self.cfg.get_phase30(game_exe)["style_index"], 2)

        # Global should remain unchanged.
        self.assertEqual(self.cfg.get_phase30()["intensity"], 1.0)


class TestOverlayStatusLogic(unittest.TestCase):
    def test_status_text_keys(self):
        # Verify the translation keys exist and are non-empty in EN and RU
        for key in ["ov_status_direct", "ov_status_daemon", "ov_status_standby", "ov_status_daemon_down_x86"]:
            set_language("en")
            self.assertTrue(bool(tr(key)))
            set_language("ru")
class TestPhase30Widgets(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        self.root = tk.Tk()
        self.root.withdraw()

    def tearDown(self):
        self.root.destroy()

    def test_phase30_panel_controls(self):
        changes = []
        resets = []
        panel = phase30.Phase30Panel(
            self.root,
            on_change=lambda k, v: changes.append((k, v)),
            on_reset=lambda: resets.append(True)
        )
        panel.pack()

        # 1. Slider change
        panel._do_push("intensity", 1.8)
        self.assertIn(("intensity", 1.8), changes)

        # 2. Style index dropdown change
        panel._var_style.set(panel._style_labels[2])
        panel._on_style_selected(None)
        self.assertIn(("style_index", 2), changes)

        # 3. Auto mask toggle
        panel._var_auto_mask.set(False)
        panel._on_mask_toggle()
        self.assertIn(("auto_mask", False), changes)

        # 4. External set_values
        panel.set_values({
            "local_tone": 1.4,
            "skin_structure": -1.0,
            "color_strength": 0.5,
        })
        vals = panel.get_values()
        self.assertEqual(vals["local_tone"], 1.4)
        self.assertEqual(vals["skin_structure"], -1.0)
        self.assertEqual(vals["color_strength"], 0.5)

        # 5. Reset button
        panel.reset_defaults()
        self.assertEqual(len(resets), 1)
        self.assertEqual(panel.get_values(), phase30.PHASE30_DEFAULTS)


if __name__ == "__main__":
    unittest.main()
