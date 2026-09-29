import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from m13 import hudmask


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
        regions = [{"shape": "ellipse", "x": 0.08, "y": 0.90,
                    "w": 0.065, "h": 0.10}]
        hudmask.write_regions(regions, self.cfg_path)
        with open(self.cfg_path, "r", encoding="ascii") as f:
            text = f.read()
        self.assertEqual(text, "ellipse 0.0800 0.9000 0.0650 0.1000\n")
        hudmask.write_regions([], self.cfg_path)
        self.assertFalse(os.path.exists(self.cfg_path))

    def test_apply_unknown_game_removes_cfg(self):
        hudmask.write_regions(
            [{"shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1}],
            self.cfg_path)
        hudmask.apply(r"C:\unknown_game.exe", path=self.cfg_path)
        self.assertFalse(os.path.exists(self.cfg_path))


if __name__ == "__main__":
    unittest.main(verbosity=2)