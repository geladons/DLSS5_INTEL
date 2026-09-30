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
from unittest.mock import patch, MagicMock, PropertyMock
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

        with tempfile.TemporaryDirectory() as flags_dir:
            flag_paths = {
                "DX9_TRIGGER": os.path.join(flags_dir, "dx9.flag"),
                "M12_PAUSE": os.path.join(flags_dir, "m12.flag"),
                "FREEZE_FLAG": os.path.join(flags_dir, "freeze.flag"),
                "REPROC_FLAG": os.path.join(flags_dir, "reproc.flag"),
            }
            with patch.multiple(gamelaunch, **flag_paths):
                gamelaunch._flag_set(gamelaunch.DX9_TRIGGER, False)
                gamelaunch._flag_set(gamelaunch.M12_PAUSE, True)
                gamelaunch._flag_set(gamelaunch.FREEZE_FLAG, True)
                gamelaunch._flag_set(gamelaunch.REPROC_FLAG, True)
                gamelaunch.reset_runtime_flags()
                assert os.path.exists(gamelaunch.DX9_TRIGGER)
                assert not os.path.exists(gamelaunch.M12_PAUSE)
                assert not os.path.exists(gamelaunch.FREEZE_FLAG)
                assert not os.path.exists(gamelaunch.REPROC_FLAG)
            print("  gamelaunch.reset_runtime_flags clears stale game state [PASS]")

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

        # A daemon still in its x86 grace period must be stopped before x64.
        with patch.object(type(ctl.daemon), "running", True):
            with patch.object(ctl, "game_deploy", return_value=(True, "deployed")), \
                 patch.object(ctl, "daemon_start") as mock_dstart, \
                 patch.object(ctl, "_ensure_daemon_stopped", return_value=(True, "stopped")) as mock_dstop, \
                 patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 111")) as mock_lg:
                ok, msg = ctl.game_launch(dummy_x64)
                assert ok, "game_launch x64 failed: %s" % msg
                mock_dstart.assert_not_called()
                mock_dstop.assert_called_once()
                mock_lg.assert_called_once_with(
                    dummy_x64, "dx11", scale=0.58, blend=0.75,
                    cfg=ctl.cfg)
                print("  controller.game_launch x64: lingering daemon stopped + config propagated [PASS]")

        # Test 32-bit game launch: SHOULD call daemon_start when daemon not running
        with patch.object(ctl, "game_deploy", return_value=(True, "deployed")), \
             patch.object(ctl, "daemon_start", return_value=(True, "daemon started")) as mock_dstart, \
             patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 222")) as mock_lg:

            ok, msg = ctl.game_launch(dummy_x86)
            assert ok, "game_launch x86 failed: %s" % msg
            mock_dstart.assert_called_once()
            mock_lg.assert_called_once_with(dummy_x86, "dx9", scale=0.58,
                                            blend=0.75, cfg=ctl.cfg)
            print("  controller.game_launch x86: TCP daemon fallback (daemon_start called) [PASS]")

    finally:
        ctl.shutdown()

with tempfile.TemporaryDirectory() as td:
    cfg = Config(os.path.join(td, "daemon_args.json"))
    ctl = M13Controller(cfg)
    try:
        with patch.object(ctl, "weights_ok", return_value=True), \
             patch.object(type(ctl.daemon), "running", new_callable=PropertyMock,
                          return_value=False), \
             patch.object(ctl.daemon, "start", return_value=(True, "started")) as mock_daemon_start:
            ok, msg = ctl.daemon_start()
            assert ok, "daemon_start failed: %s" % msg
            args = mock_daemon_start.call_args[0][0]
            values = dict(zip(args[::2], args[1::2]))
            assert values["--intensity"] == "1.000"
            assert values["--local-tone"] == "1.000"
            assert values["--local-structure"] == "1.000"
            assert values["--skin-structure"] == "-1.000"
            assert values["--auto-mask"] == "0"
            assert "--gain" not in args and "--blend" not in args
            print("  controller.daemon_start passes public Phase30 values separately from legacy gain [PASS]")
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

print("\n=== 7. Testing Task B: Overlay Mouse Focus & Freeze, UI Restoration, and Daemon Lifecycle ===")
# 7.1 Test Overlay show/hide freeze lifecycle and cursor configuration
import tkinter as tk
tk_root = tk.Tk()
tk_root.withdraw()
try:
    with tempfile.TemporaryDirectory() as td:
        c = Config(os.path.join(td, "ov_cfg.json"))
        c_ctl = M13Controller(c)
        try:
            ov = overlay.ControlOverlay(tk_root, c_ctl, on_log=lambda m: None)
            assert ov.win.cget("cursor") == "arrow", "Overlay window cursor != arrow"

            def _tree(w):
                out = [w]
                for ch in w.winfo_children():
                    out.extend(_tree(ch))
                return out

            # F1: overlay_freeze False -> show() never freezes; label empty.
            with patch("m13.overlay._set_noactivate") as noactivate, \
                 patch("m13.overlay._force_foreground"), \
                 patch.object(gamelaunch, "freeze_set") as m_freeze:
                ov.show()
                noactivate.assert_called_once()
                assert noactivate.call_args[0][1] is True
                assert not any(ca.args and ca.args[0] is True
                               for ca in m_freeze.call_args_list), \
                    "freeze_set(True) called with overlay_freeze=False"
            assert ov.freeze_lbl.cget("text") == "", "freeze label not empty"
            assert ov._freeze_flags is False, "ov._freeze_flags set without opt-in"
            assert ov._focus_mode is False, "default overlay unexpectedly took focus"
            ov.hide()
            print("  F1 overlay_freeze=False: show() never freezes, label empty [PASS]")

            # F2: overlay_freeze True -> freeze on show, release on hide.
            c.set("overlay_freeze", True)
            with patch("m13.overlay._set_noactivate"), \
                 patch.object(gamelaunch, "freeze_set") as m_freeze:
                ov.show()
                m_freeze.assert_called_once_with(True)
                ov.hide()
                m_freeze.assert_any_call(False)
            print("  F2 overlay_freeze=True: freeze on show, release on hide [PASS]")
            c.set("overlay_freeze", False)

            # F3: overlay_focus False -> no activation path runs at all and
            # every widget's takefocus is stripped.
            with patch("m13.overlay._force_foreground") as m_ff, \
                 patch.object(ov.win, "focus_force") as m_fc, \
                 patch.object(overlay._u32, "SetForegroundWindow") as m_sfw:
                ov.show()
                m_ff.assert_not_called()
                m_fc.assert_not_called()
                m_sfw.assert_not_called()
            assert ov._focus_mode is False
            for w in _tree(ov.win):
                try:
                    tf = w.cget("takefocus")
                except tk.TclError:
                    continue
                assert tf in ("0", 0, False), \
                    "widget %s kept takefocus=%r" % (w, tf)
            print("  F3 overlay_focus=False: no steal calls, takefocus stripped [PASS]")
            ov.hide()

            # F4: overlay_focus True -> foreground taken once and returned
            # to the game hwnd on hide.
            c.set("overlay_focus", True)
            with patch.object(c_ctl, "active_game_pid", return_value=1234), \
                 patch("m13.overlay.hwnd_for_pid", return_value=0xABC), \
                 patch("m13.overlay._set_noactivate") as noactivate, \
                 patch("m13.overlay._force_foreground") as m_ff, \
                 patch.object(overlay._u32, "SetForegroundWindow") as m_sfw:
                ov.show()
                noactivate.assert_called_once()
                assert noactivate.call_args[0][1] is False
                m_ff.assert_called_once()
                assert ov._focus_mode is True
                ov.hide()
                m_sfw.assert_called_with(0xABC)
            print("  F4 overlay_focus=True: steal on show, restore on hide [PASS]")
            c.set("overlay_focus", False)

            # F5: _take_focus() without overlay_focus is inert.
            with patch.object(ov.win, "focus_set") as m_fs:
                ov._take_focus()
                m_fs.assert_not_called()
            assert ov._focus_mode is False
            print("  F5 _take_focus() inert when overlay_focus=False [PASS]")
        finally:
            c_ctl.shutdown()
finally:
    tk_root.destroy()

# 7.2 Test StateMonitor auto-stops daemon when running game process terminates
with tempfile.TemporaryDirectory() as td:
    dummy_x86 = os.path.join(td, "test_x86.exe")
    with open(dummy_x86, "w") as f:
        f.write("dummy")
    c = Config(os.path.join(td, "daemon_exit_cfg.json"))
    c.set("games", {
        dummy_x86: {"mode": "dx9", "arch": "x86", "name": "TestX86"}
    })
    c_ctl = M13Controller(c)
    try:
        from unittest.mock import PropertyMock
        with patch.object(type(c_ctl.daemon), "running", new_callable=PropertyMock, return_value=True), \
             patch.object(c_ctl.daemon, "stop") as mock_dstop:
            # First pass: game is running (PID 9999)
            with patch("m13.controller.find_pid", return_value=9999):
                snap = c_ctl.monitor._collect()
                assert snap["games"][dummy_x86]["running"] is True
                mock_dstop.assert_not_called()

            # Second pass: game has terminated (find_pid returns None, grace period 10 ticks)
            with patch("m13.controller.find_pid", return_value=None):
                for _ in range(10):
                    snap = c_ctl.monitor._collect()
                assert snap["games"][dummy_x86]["running"] is False
                mock_dstop.assert_called_once()
                assert snap["daemon_pid"] is None, "snap['daemon_pid'] should be None after auto-stop"
                print("  StateMonitor._collect auto-stops x86 daemon when game exits [PASS]")

        with patch.object(type(c_ctl.daemon), "running", new_callable=PropertyMock, return_value=True), \
             patch.object(c_ctl.daemon, "stop") as mock_dstop, \
             patch("m13.controller.find_pid", return_value=None):
            for _ in range(12):
                c_ctl.monitor._collect()
            mock_dstop.assert_not_called()
            print("  StateMonitor keeps daemon during delayed x86 game startup [PASS]")
    finally:
        c_ctl.shutdown()

# 7.3 Test UI restoration method logic
class DummyUI:
    def __init__(self):
        self.root = MagicMock()
        self.games_tab = MagicMock()
        self._on_focus_in = ui.ManagerUI._on_focus_in.__get__(self, DummyUI)

from m13 import ui
dummy_ui = DummyUI()
# When window dimensions shrunk below minimums (e.g. 800x600 after GTA IV fullscreen)
dummy_ui.root.winfo_width.return_value = 800
dummy_ui.root.winfo_height.return_value = 600
dummy_ui._on_focus_in()
dummy_ui.root.geometry.assert_called_with("1024x760")
dummy_ui.root.update_idletasks.assert_called_once()
dummy_ui.games_tab.cards_fill.assert_called_once()
print("  ui._on_focus_in detects shrunken window and restores 1024x760 layout [PASS]")

# 7.4 Test Screen Mode strings and clarity
from m13.i18n import tr
s_note = tr("screen_note")
assert "m8blive.exe" in s_note, "screen_note missing m8blive.exe reference"
assert "m11d.exe" in s_note, "screen_note missing m11d.exe reference"
print("  screen_note clarity for standalone direct DDA overlay verified [PASS]")

print("\nALL STAGE 5 & TASK B UNIT TESTS PASSED SUCCESSFULLY!")
