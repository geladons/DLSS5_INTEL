# Stage 5 Unit Tests: UI Presets, Frame Generation Controls, Packaging
import os
import sys
import tempfile

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.config import Config, DEFAULTS
from m13.i18n import (set_language, get_language, tr,
                      scale_preset_labels, scale_to_preset_label,
                      preset_label_to_scale)

print("=== 1. Testing DEFAULTS ===")
assert "scale" in DEFAULTS, "Missing scale in DEFAULTS"
assert DEFAULTS["scale"] == 0.50, "scale default != 0.50"
assert "frame_gen" in DEFAULTS, "Missing frame_gen in DEFAULTS"
assert DEFAULTS["frame_gen"] is True, "frame_gen default != True"
assert "sharpness" in DEFAULTS, "Missing sharpness in DEFAULTS"
assert DEFAULTS["sharpness"] == 0.8, "sharpness default != 0.8"
print("DEFAULTS: scale=0.50, frame_gen=True, sharpness=0.8 [PASS]")

print("\n=== 2. Testing i18n keys ===")
keys_to_test = [
    "scale", "scale_native", "scale_quality", "scale_balanced",
    "scale_perf", "scale_ultra", "framegen", "framegen_hint", "sharpness"
]
for lang in ["en", "ru"]:
    set_language(lang)
    for k in keys_to_test:
        val = tr(k)
        assert val != k, ("Missing translation for %s in %s" % (k, lang))
        safe_val = val.encode("ascii", "replace").decode("ascii")
        print("  [%s] %s: %s" % (lang, k, safe_val))

    for exp_val, k in [(1.00, "scale_native"), (0.67, "scale_quality"),
                       (0.58, "scale_balanced"), (0.50, "scale_perf"),
                       (0.33, "scale_ultra")]:
        lbl = scale_to_preset_label(exp_val)
        got_val = preset_label_to_scale(lbl)
        assert abs(got_val - exp_val) < 1e-4, (
            "Roundtrip failed for %s: %s -> %s" % (exp_val, lbl, got_val))
set_language("en")
print("i18n keys and preset mapping roundtrips [PASS]")

print("\n=== 3. Testing Config serialization/deserialization ===")
with tempfile.TemporaryDirectory() as td:
    cfg_file = os.path.join(td, "test_cfg.json")
    c1 = Config(cfg_file)
    assert c1.get("scale") == 0.50
    assert c1.get("frame_gen") is True
    assert c1.get("sharpness") == 0.8

    c1.set("scale", 0.67)
    c1.set("frame_gen", False)
    c1.set("sharpness", 0.95)

    c2 = Config(cfg_file)
    assert c2.get("scale") == 0.67, "Config reload scale mismatch"
    assert c2.get("frame_gen") is False, "Config reload frame_gen mismatch"
    assert c2.get("sharpness") == 0.95, "Config reload sharpness mismatch"
    print("Config persistence verified [PASS]")

print("\n=== 4. Testing Controller methods ===")
from m13.controller import M13Controller

with tempfile.TemporaryDirectory() as td:
    cfg_file = os.path.join(td, "ctl_cfg.json")
    cfg = Config(cfg_file)
    ctl = M13Controller(cfg)
    try:
        ok, msg = ctl.set_scale(0.58)
        assert ok, "set_scale failed"
        assert ctl.cfg.get("scale") == 0.58, "cfg scale mismatch"
        assert os.environ.get("DLSS5_SCALE") == "0.58", "DLSS5_SCALE env mismatch"
        assert os.environ.get("NR_LAYER_SCALE") == "0.58", "NR_LAYER_SCALE env mismatch"
        assert os.environ.get("M12_SCALE") == "0.58", "M12_SCALE env mismatch"
        print("  set_scale(0.58) -> %s [PASS]" % msg)

        # clamping
        ctl.set_scale(0.1)
        assert ctl.cfg.get("scale") == 0.33, "clamping min failed"
        ctl.set_scale(1.5)
        assert ctl.cfg.get("scale") == 1.0, "clamping max failed"
        print("  set_scale clamping (0.33..1.0) [PASS]")

        ok, msg = ctl.set_framegen(False)
        assert ok, "set_framegen(False) failed"
        assert ctl.cfg.get("frame_gen") is False, "cfg frame_gen mismatch"
        assert os.environ.get("DLSS5_FRAMEGEN") == "0", "DLSS5_FRAMEGEN env mismatch"
        print("  set_framegen(False) -> %s [PASS]" % msg)

        ok, msg = ctl.set_framegen(True)
        assert ok, "set_framegen(True) failed"
        assert ctl.cfg.get("frame_gen") is True, "cfg frame_gen mismatch"
        assert os.environ.get("DLSS5_FRAMEGEN") == "1", "DLSS5_FRAMEGEN env mismatch"
        print("  set_framegen(True) -> %s [PASS]" % msg)

        ok, msg = ctl.set_sharpness(0.75)
        assert ok, "set_sharpness(0.75) failed"
        assert ctl.cfg.get("sharpness") == 0.75, "cfg sharpness mismatch"
        assert os.environ.get("DLSS5_SHARPNESS") == "0.75", "DLSS5_SHARPNESS env mismatch"
        print("  set_sharpness(0.75) -> %s [PASS]" % msg)

        # clamping
        ctl.set_sharpness(-0.5)
        assert ctl.cfg.get("sharpness") == 0.0, "sharpness min clamping failed"
        ctl.set_sharpness(2.0)
        assert ctl.cfg.get("sharpness") == 1.0, "sharpness max clamping failed"
        print("  set_sharpness clamping (0.0..1.0) [PASS]")

        snap = ctl.snapshot()
        assert "scale" in snap, "Missing scale in snapshot"
        assert "frame_gen" in snap, "Missing frame_gen in snapshot"
        assert "sharpness" in snap, "Missing sharpness in snapshot"
        print("  StateMonitor snapshot contains scale, frame_gen, sharpness [PASS]")
    finally:
        ctl.shutdown()

print("\nALL STAGE 5 UNIT TESTS PASSED SUCCESSFULLY!")
