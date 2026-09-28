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

print("\n=== 5. Testing Game Launching and Arch-Aware Daemon Handling ===")
import subprocess
from unittest.mock import patch, MagicMock
from m13 import gamelaunch

# 5.1 Test gamelaunch._launch flags and stdio
with patch("subprocess.Popen") as mock_popen:
    mock_proc = MagicMock()
    mock_proc.pid = 12345
    mock_popen.return_value = mock_proc

    with tempfile.TemporaryDirectory() as td:
        dummy_exe = os.path.join(td, "game.exe")
        with open(dummy_exe, "w") as f:
            f.write("dummy")

        ok, msg = gamelaunch.launch_game(dummy_exe, "dx9", scale=0.67, gain=1.25, blend=0.8)
        assert ok, "launch_game failed: %s" % msg
        assert "12345" in msg, "PID 12345 not in launch_game message: %s" % msg

        mock_popen.assert_called_once()
        args, kwargs = mock_popen.call_args
        assert kwargs.get("creationflags") == subprocess.CREATE_NEW_PROCESS_GROUP, (
            "creationflags != CREATE_NEW_PROCESS_GROUP: %s" % kwargs.get("creationflags"))
        assert (kwargs.get("creationflags") & 0x00000008) == 0, "DETACHED_PROCESS flag still present!"
        assert kwargs.get("stdin") == subprocess.DEVNULL, "stdin != DEVNULL"
        assert kwargs.get("stdout") == subprocess.DEVNULL, "stdout != DEVNULL"
        assert kwargs.get("stderr") == subprocess.DEVNULL, "stderr != DEVNULL"
        assert kwargs.get("close_fds") is True, "close_fds != True"
        print("  gamelaunch._launch process flags and DEVNULL stdio [PASS]")

        # 5.2 Test environment propagation
        env = kwargs.get("env")
        assert env is not None, "env is None"
        assert env.get("ENABLE_NR_LAYER") == "1", "ENABLE_NR_LAYER mismatch"
        assert env.get("NR_LAYER_LIVE") == "1", "NR_LAYER_LIVE mismatch"
        assert env.get("NR_LAYER_TRIGGER") == gamelaunch.DX9_TRIGGER, "NR_LAYER_TRIGGER mismatch"
        assert env.get("NR_LAYER_FREEZE") == gamelaunch.FREEZE_FLAG, "NR_LAYER_FREEZE mismatch"
        assert env.get("NR_LAYER_REPROC") == gamelaunch.REPROC_FLAG, "NR_LAYER_REPROC mismatch"

        assert env.get("NR_LAYER_SCALE") == "0.67" and env.get("DLSS5_SCALE") == "0.67" and env.get("M12_SCALE") == "0.67", "Scale env mismatch"
        assert env.get("NR_LAYER_GAIN") == "1.25" and env.get("DLSS5_GAIN") == "1.25" and env.get("M12_GAIN") == "1.25", "Gain env mismatch"
        assert env.get("NR_LAYER_BLEND") == "0.8" and env.get("DLSS5_BLEND") == "0.8" and env.get("M12_BLEND") == "0.8", "Blend env mismatch"
        assert env.get("NR_LAYER_UI_MASK") == "1", "NR_LAYER_UI_MASK mismatch"
        assert env.get("DLSS5_SHARPNESS") == "0.8", "DLSS5_SHARPNESS mismatch"
        print("  gamelaunch.launch_game env propagation (always-injected + scale/gain/blend/ui_mask/sharpness) [PASS]")

        # 5.3 Test trigger flag existence for layer modes
        assert os.path.exists(gamelaunch.DX9_TRIGGER), "DX9_TRIGGER flag not set for dx9"
        print("  gamelaunch trigger flag set for layer mode [PASS]")

# 5.4 Test controller.game_launch architecture-aware daemon handling
with tempfile.TemporaryDirectory() as td:
    cfg_file = os.path.join(td, "ctl_cfg.json")
    cfg = Config(cfg_file)
    cfg.set("scale", 0.58)
    cfg.set("gain", 1.5)
    cfg.set("blend", 0.75)
    ctl = M13Controller(cfg)
    try:
        dummy_x64 = os.path.join(td, "game_x64.exe")
        with open(dummy_x64, "w") as f:
            f.write("dummy")
        dummy_x86 = os.path.join(td, "game_x86.exe")
        with open(dummy_x86, "w") as f:
            f.write("dummy")

        ctl.cfg.set("games", {
            dummy_x64: {"mode": "dx11", "arch": "x64", "name": "Game64"},
            dummy_x86: {"mode": "dx9", "arch": "x86", "name": "Game32"},
        })

        # Test 64-bit game launch: should NOT call daemon_start
        with patch.object(ctl, "game_deploy", return_value=(True, "deployed")), \
             patch.object(ctl, "daemon_start") as mock_dstart, \
             patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 111")) as mock_lg:

            ok, msg = ctl.game_launch(dummy_x64)
            assert ok, "game_launch x64 failed: %s" % msg
            mock_dstart.assert_not_called()
            mock_lg.assert_called_once_with(dummy_x64, "dx11", scale=0.58, gain=1.5, blend=0.75)
            print("  controller.game_launch x64: In-Process (daemon NOT started) + config propagated [PASS]")

        # Test 32-bit game launch: SHOULD call daemon_start when daemon not running
        with patch.object(ctl, "game_deploy", return_value=(True, "deployed")), \
             patch.object(ctl, "daemon_start", return_value=(True, "daemon started")) as mock_dstart, \
             patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 222")) as mock_lg:

            ok, msg = ctl.game_launch(dummy_x86)
            assert ok, "game_launch x86 failed: %s" % msg
            mock_dstart.assert_called_once()
            mock_lg.assert_called_once_with(dummy_x86, "dx9", scale=0.58, gain=1.5, blend=0.75)
            print("  controller.game_launch x86: TCP daemon fallback (daemon_start called) [PASS]")
    finally:
        ctl.shutdown()

print("\n=== 6. Testing Task 2: Overlay Non-Minimizing & Photo-Mode Freeze ===")
assert "overlay_focus" in DEFAULTS, "Missing overlay_focus in DEFAULTS"
assert DEFAULTS["overlay_focus"] is False, "overlay_focus default must be False"
print("  DEFAULTS['overlay_focus'] == False [PASS]")

# Test freeze_supported for all active modes
with tempfile.TemporaryDirectory() as td:
    cfg = Config(os.path.join(td, "freeze_cfg.json"))
    ctl = M13Controller(cfg)
    try:
        for mode in ("dx9", "dx11", "dx12", "vulkan"):
            with patch.object(ctl, "snapshot", return_value={"active_mode": mode}):
                assert ctl.freeze_supported() is True, "freeze_supported should be True for %s" % mode
        for mode in ("screen", None, "unknown"):
            with patch.object(ctl, "snapshot", return_value={"active_mode": mode}):
                assert ctl.freeze_supported() is False, "freeze_supported should be False for %s" % mode
        print("  ctl.freeze_supported() True for dx9/dx11/dx12/vulkan without launched_exes [PASS]")

        # Test set_blend env propagation and reproc bump
        gamelaunch.freeze_set(True)
        try:
            ok, msg = ctl.set_blend(0.65)
            assert ok, "set_blend failed"
            assert os.environ.get("DLSS5_BLEND") == "0.65", "DLSS5_BLEND mismatch"
            assert os.environ.get("NR_LAYER_BLEND") == "0.65", "NR_LAYER_BLEND mismatch"
            assert os.environ.get("M12_BLEND") == "0.65", "M12_BLEND mismatch"
            assert "frozen" in msg, "set_blend should indicate frozen frame reprocessing: %s" % msg
            print("  ctl.set_blend env propagation and reproc bump on freeze [PASS]")
        finally:
            gamelaunch.freeze_set(False)
    finally:
        ctl.shutdown()

# Test overlay window styles & focus behavior
from m13 import overlay
assert overlay.WS_EX_TOPMOST == 0x00000008, "WS_EX_TOPMOST incorrect"
assert overlay.WS_EX_NOACTIVATE == 0x08000000, "WS_EX_NOACTIVATE incorrect"
assert overlay.WS_EX_TOOLWINDOW == 0x00000080, "WS_EX_TOOLWINDOW incorrect"
print("  overlay style constants verified [PASS]")

print("\nALL STAGE 5 UNIT TESTS PASSED SUCCESSFULLY!")
