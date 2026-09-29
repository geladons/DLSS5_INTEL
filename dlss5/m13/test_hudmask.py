import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from m13 import hudmask
from m13 import gamelaunch


class HudMaskTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="hudmask_test_")
        self.cfg_path = os.path.join(self.tmp, "m13_hudmask.cfg")

    def test_validate_filters_garbage(self):
        raw = [
            {"shape": "rect", "x": 0.1, "y": 0.2, "w": 0.3, "h": 0.4},
            {"shape": "circle", "x": 0, "y": 0, "w": 1, "h": 1},
            {"shape": "ellipse", "x": -0.5, "y": 2.0, "w": 0.2, "h": "z"},
            "not a dict",
        ]
        out = hudmask.validate(raw)
        self.assertEqual(len(out), 2)
        self.assertEqual(out[0]["shape"], "rect")
        self.assertEqual(out[1]["x"], 0.0)
        self.assertEqual(out[1]["y"], 1.0)
        self.assertEqual(out[1]["h"], 0.0)

    def test_resolve_preset_by_basename(self):
        regions = hudmask.resolve(r"C:\Games\GTA IV\gtaiv.exe")
        self.assertTrue(regions)
        self.assertEqual(regions[0]["shape"], "ellipse")

    def test_resolve_unknown_game_empty(self):
        self.assertEqual(hudmask.resolve(r"C:\Games\rdr2.exe"), [])

    def test_per_game_override_wins(self):
        override = [{"shape": "rect", "x": 0.5, "y": 0.5, "w": 0.1, "h": 0.1}]
        cfg = {"games": {r"C:\gtaiv.exe": {"hudmask": override}}}
        regions = hudmask.resolve(r"C:\gtaiv.exe", cfg)
        self.assertEqual(regions[0]["x"], 0.5)

    def test_empty_override_disables_preset(self):
        cfg = {"games": {r"C:\gtaiv.exe": {"hudmask": []}}}
        self.assertEqual(hudmask.resolve(r"C:\gtaiv.exe", cfg), [])

    def test_write_and_remove(self):
        regions = [{"shape": "ellipse", "x": 0.075, "y": 0.92,
                    "w": 0.055, "h": 0.075}]
        hudmask.write_regions(regions, self.cfg_path)
        with open(self.cfg_path, "r", encoding="ascii") as f:
            text = f.read()
        self.assertEqual(text, "ellipse 0.0750 0.9200 0.0550 0.0750\n")
        hudmask.write_regions([], self.cfg_path)
        self.assertFalse(os.path.exists(self.cfg_path))

    def test_apply_unknown_game_removes_cfg(self):
        hudmask.write_regions(
            [{"shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1}],
            self.cfg_path)
        hudmask.apply(r"C:\unknown_game.exe", path=self.cfg_path)
        self.assertFalse(os.path.exists(self.cfg_path))

    def test_gtaiv_preset_tightened(self):
        regions = hudmask.resolve(r"C:\Games\GTA IV\gtaiv.exe")
        self.assertEqual(len(regions), 4)
        self.assertEqual(regions[0]["shape"], "ellipse")
        self.assertAlmostEqual(regions[0]["x"], 0.075)
        self.assertAlmostEqual(regions[0]["y"], 0.92)
        self.assertAlmostEqual(regions[0]["w"], 0.055)
        self.assertAlmostEqual(regions[0]["h"], 0.075)
        total_area = 0.0
        for r in regions:
            if r["shape"] == "rect":
                total_area += r["w"] * r["h"]
            elif r["shape"] == "ellipse":
                total_area += 3.14159265 * r["w"] * r["h"]
        self.assertLess(total_area, 0.10)


class LaunchGameMaskGatingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="hudmask_gate_")
        self.cfg_path = os.path.join(self.tmp, "m13_hudmask.cfg")
        self.fake_exe = os.path.join(self.tmp, "gtaiv.exe")
        with open(self.fake_exe, "w") as f:
            f.write("fake")

    @patch("m13.gamelaunch._launch")
    def test_auto_mask_false_removes_file(self, mock_launch):
        mock_launch.return_value = (True, "launched pid 1234")
        cfg = {
            "auto_mask": False,
            "sharpness": 0.8,
            "frame_gen": True,
        }
        with patch.object(hudmask, "HUDMASK_FLAG", self.cfg_path):
            hudmask.write_regions(
                [{"shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1}],
                self.cfg_path)
            self.assertTrue(os.path.exists(self.cfg_path))
            gamelaunch.launch_game(self.fake_exe, "dx9", cfg=cfg)
            self.assertFalse(os.path.exists(self.cfg_path))
            env_extra = mock_launch.call_args[0][1]
            self.assertIn("NR_LAYER_HUDMASK", env_extra)
            self.assertEqual(env_extra["NR_LAYER_HUDMASK"], self.cfg_path)

    @patch("m13.gamelaunch._launch")
    def test_auto_mask_true_writes_file(self, mock_launch):
        mock_launch.return_value = (True, "launched pid 1234")
        cfg = {
            "auto_mask": True,
            "sharpness": 0.8,
            "frame_gen": True,
        }
        with patch.object(hudmask, "HUDMASK_FLAG", self.cfg_path):
            gamelaunch.launch_game(self.fake_exe, "dx9", cfg=cfg)
            self.assertTrue(os.path.exists(self.cfg_path))
            env_extra = mock_launch.call_args[0][1]
            self.assertIn("NR_LAYER_HUDMASK", env_extra)

    @patch("m13.gamelaunch._launch")
    def test_auto_mask_with_config_object(self, mock_launch):
        mock_launch.return_value = (True, "launched pid 1234")

        class FakeCfg:
            def get(self, key, default=None):
                vals = {"sharpness": 0.8, "frame_gen": True}
                return vals.get(key, default)

            def get_phase30(self, exe=None):
                return {"auto_mask": True, "intensity": 1.0,
                        "local_tone": 1.0, "local_structure": 1.0,
                        "skin_structure": -1.0, "style_index": 0,
                        "color_strength": 1.0}

        with patch.object(hudmask, "HUDMASK_FLAG", self.cfg_path):
            gamelaunch.launch_game(self.fake_exe, "dx9", cfg=FakeCfg())
            self.assertTrue(os.path.exists(self.cfg_path))


class LiveToggleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="hudmask_toggle_")
        self.cfg_path = os.path.join(self.tmp, "m13_hudmask.cfg")

    def test_toggle_on_writes_regions(self):
        with patch.object(hudmask, "HUDMASK_FLAG", self.cfg_path):
            hudmask.write_regions([], self.cfg_path)
            self.assertFalse(os.path.exists(self.cfg_path))
            regions = hudmask.resolve(r"C:\Games\GTA IV\gtaiv.exe")
            hudmask.write_regions(regions, self.cfg_path)
            self.assertTrue(os.path.exists(self.cfg_path))
            with open(self.cfg_path, "r", encoding="ascii") as f:
                lines = f.read().strip().split("\n")
            self.assertEqual(len(lines), 4)

    def test_toggle_off_deletes_file(self):
        with patch.object(hudmask, "HUDMASK_FLAG", self.cfg_path):
            regions = hudmask.resolve(r"C:\Games\GTA IV\gtaiv.exe")
            hudmask.write_regions(regions, self.cfg_path)
            self.assertTrue(os.path.exists(self.cfg_path))
            hudmask.write_regions([], self.cfg_path)
            self.assertFalse(os.path.exists(self.cfg_path))


if __name__ == "__main__":
    unittest.main(verbosity=2)
