import os
import sys
import tempfile
import time
from unittest.mock import patch, MagicMock, PropertyMock

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.config import Config
from m13.controller import M13Controller
from m13 import gamelaunch


def _make_ctl(td):
    cfg_file = os.path.join(td, "test_cfg.json")
    cfg = Config(cfg_file)
    cfg.set("scale", 0.58)
    cfg.set("blend", 0.75)
    return M13Controller(cfg), cfg


print("=== Test A: x64 launch succeeds after force-kill fallback ===")
with tempfile.TemporaryDirectory() as td:
    ctl, cfg = _make_ctl(td)
    try:
        dummy_x64 = os.path.join(td, "game_x64.exe")
        with open(dummy_x64, "w") as f:
            f.write("dummy")
        cfg.set("games", {
            dummy_x64: {"mode": "vulkan", "arch": "x64", "name": "TestX64"},
        })
        running_state = [True]

        def fake_stop():
            return False, "graceful stop failed"

        def fake_running(self_inner):
            return running_state[0]

        def fake_pid(self_inner):
            return 12345 if running_state[0] else None

        def fake_kill(pid, force=True):
            running_state[0] = False
            return True, "killed"

        with patch.object(type(ctl.daemon), "running", property(fake_running)), \
             patch.object(type(ctl.daemon), "pid", property(fake_pid)), \
             patch.object(ctl.daemon, "stop", side_effect=fake_stop), \
             patch.object(ctl, "game_deploy", return_value=(True, "deployed")), \
             patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 999")) as mock_lg, \
             patch("m13.controller.time.sleep"), \
             patch("m13.processes.stop_pid", side_effect=fake_kill) as mock_stop_pid:
            ok, msg = ctl.game_launch(dummy_x64)
            assert ok, "game_launch should succeed after force-kill: %s" % msg
            mock_stop_pid.assert_called_once_with(12345, force=True)
            mock_lg.assert_called_once()
            print("  x64 launch after force-kill fallback [PASS]")
    finally:
        ctl.shutdown()


print("\n=== Test B: Witcher 3 dx11 -> dx12 sibling switch ===")
with tempfile.TemporaryDirectory() as td:
    ctl, cfg = _make_ctl(td)
    try:
        dx11_dir = os.path.join(td, "bin", "x64")
        dx12_dir = os.path.join(td, "bin", "x64_dx12")
        os.makedirs(dx11_dir)
        os.makedirs(dx12_dir)
        w3_dx11 = os.path.join(dx11_dir, "witcher3.exe")
        w3_dx12 = os.path.join(dx12_dir, "witcher3.exe")
        with open(w3_dx11, "w") as f:
            f.write("dummy")
        with open(w3_dx12, "w") as f:
            f.write("dummy")
        cfg.set("games", {
            w3_dx11: {"mode": "dx11", "arch": "x64", "name": "Witcher3"},
        })
        resolved_exe, resolved_meta = ctl._resolve_witcher3_dx12(
            w3_dx11, cfg.get("games")[w3_dx11], cfg)
        assert resolved_exe == w3_dx12, "expected dx12 sibling, got: %s" % resolved_exe
        assert resolved_meta["mode"] == "dx12", "mode should be dx12, got: %s" % resolved_meta["mode"]
        games = cfg.get("games")
        assert w3_dx12 in games, "dx12 exe should be in config after switch"
        assert w3_dx11 not in games, "dx11 exe should be removed from config after switch"
        print("  witcher3 dx11->dx12 sibling switch [PASS]")

        no_sibling_dir = os.path.join(td, "other", "bin", "x64")
        os.makedirs(no_sibling_dir)
        w3_nosib = os.path.join(no_sibling_dir, "witcher3.exe")
        with open(w3_nosib, "w") as f:
            f.write("dummy")
        meta_orig = {"mode": "dx11", "arch": "x64", "name": "Witcher3"}
        resolved_exe2, resolved_meta2 = ctl._resolve_witcher3_dx12(
            w3_nosib, meta_orig, cfg)
        assert resolved_exe2 == w3_nosib, "should keep original when no sibling"
        assert resolved_meta2["mode"] == "dx11", "should keep dx11 when no sibling"
        print("  witcher3 no-sibling keeps dx11 [PASS]")

        not_w3 = os.path.join(td, "rdr2.exe")
        with open(not_w3, "w") as f:
            f.write("dummy")
        meta_rdr = {"mode": "vulkan", "arch": "x64", "name": "RDR2"}
        r_exe, r_meta = ctl._resolve_witcher3_dx12(not_w3, meta_rdr, cfg)
        assert r_exe == not_w3
        assert r_meta["mode"] == "vulkan"
        print("  non-witcher3 exe unchanged [PASS]")
    finally:
        ctl.shutdown()


print("\n=== Test C: Missing launch_exe falls back to main exe ===")
with tempfile.TemporaryDirectory() as td:
    ctl, cfg = _make_ctl(td)
    try:
        main_exe = os.path.join(td, "RDR2.exe")
        with open(main_exe, "w") as f:
            f.write("dummy")
        missing_launcher = os.path.join(td, "Launcher.exe")
        meta = {"mode": "vulkan", "arch": "x64", "name": "RDR2",
                "launch_exe": missing_launcher}
        target = ctl.launch_target(main_exe, meta)
        assert target == main_exe, "should fall back to main exe, got: %s" % target
        print("  missing launch_exe falls back to main exe [PASS]")

        existing_launcher = os.path.join(td, "Launcher.exe")
        with open(existing_launcher, "w") as f:
            f.write("dummy")
        meta2 = {"mode": "vulkan", "arch": "x64", "name": "RDR2",
                 "launch_exe": existing_launcher}
        target2 = ctl.launch_target(main_exe, meta2)
        assert target2 == existing_launcher, "should use launcher when it exists"
        print("  existing launch_exe used correctly [PASS]")

        meta3 = {"mode": "vulkan", "arch": "x64", "name": "RDR2"}
        target3 = ctl.launch_target(main_exe, meta3)
        assert target3 == main_exe, "no launch_exe should use main exe"
        print("  no launch_exe uses main exe [PASS]")
    finally:
        ctl.shutdown()


print("\n=== Test D: Full game_launch integration with witcher3 switch ===")
with tempfile.TemporaryDirectory() as td:
    ctl, cfg = _make_ctl(td)
    try:
        dx11_dir = os.path.join(td, "bin", "x64")
        dx12_dir = os.path.join(td, "bin", "x64_dx12")
        os.makedirs(dx11_dir)
        os.makedirs(dx12_dir)
        w3_dx11 = os.path.join(dx11_dir, "witcher3.exe")
        w3_dx12 = os.path.join(dx12_dir, "witcher3.exe")
        with open(w3_dx11, "w") as f:
            f.write("dummy")
        with open(w3_dx12, "w") as f:
            f.write("dummy")
        cfg.set("games", {
            w3_dx11: {"mode": "dx11", "arch": "x64", "name": "Witcher3"},
        })
        with patch.object(type(ctl.daemon), "running", new_callable=PropertyMock, return_value=False), \
             patch.object(ctl, "game_deploy", return_value=(True, "deployed")) as mock_deploy, \
             patch("m13.gamelaunch.launch_game", return_value=(True, "launched pid 777")) as mock_lg:
            ok, msg = ctl.game_launch(w3_dx11)
            assert ok, "game_launch witcher3 failed: %s" % msg
            deploy_exe = mock_deploy.call_args[0][0]
            assert deploy_exe == w3_dx12, "deploy should use dx12 exe, got: %s" % deploy_exe
            lg_args = mock_lg.call_args
            assert lg_args[0][0] == w3_dx12, "launch should use dx12 exe"
            assert lg_args[0][1] == "dx12", "launch mode should be dx12, got: %s" % lg_args[0][1]
            print("  full game_launch witcher3 dx12 integration [PASS]")
    finally:
        ctl.shutdown()


print("\nALL LAUNCH PATH TESTS PASSED!")
