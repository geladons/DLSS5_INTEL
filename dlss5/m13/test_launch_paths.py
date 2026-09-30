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
from m13.launcher import GameLauncher


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
        target = GameLauncher.launch_target(main_exe, meta)
        assert target == main_exe, "should fall back to main exe, got: %s" % target
        print("  missing launch_exe falls back to main exe [PASS]")

        existing_launcher = os.path.join(td, "Launcher.exe")
        with open(existing_launcher, "w") as f:
            f.write("dummy")
        meta2 = {"mode": "vulkan", "arch": "x64", "name": "RDR2",
                 "launch_exe": existing_launcher}
        target2 = GameLauncher.launch_target(main_exe, meta2)
        assert target2 == existing_launcher, "should use launcher when it exists"
        print("  existing launch_exe used correctly [PASS]")

        meta3 = {"mode": "vulkan", "arch": "x64", "name": "RDR2"}
        target3 = GameLauncher.launch_target(main_exe, meta3)
        assert target3 == main_exe, "no launch_exe should use main exe"
        print("  no launch_exe uses main exe [PASS]")
    finally:
        ctl.shutdown()


print("\n=== Test D: Deployer.deploy_mode does NOT touch sister renderer dirs ===")
with tempfile.TemporaryDirectory() as td:
    from m13 import deploy as deploy_mod
    x64_dir = os.path.join(td, "bin", "x64")
    dx12_dir = os.path.join(td, "bin", "x64_dx12")
    os.makedirs(x64_dir)
    os.makedirs(dx12_dir)
    # dummy source dlls so deploy_mode never reaches for the real bundle
    src_dir = os.path.join(td, "srcdlls")
    os.makedirs(src_dir)

    def fake_source(mode, arch, dll_name):
        p = os.path.join(src_dir, "%s_%s" % (mode, dll_name))
        if not os.path.exists(p):
            with open(p, "wb") as f:
                f.write(b"fake-%s" % dll_name.encode())
        return p

    with patch.object(deploy_mod, "mode_dll_source", side_effect=fake_source):
        out = deploy_mod.Deployer(x64_dir).deploy_mode("dx11", "x64")
    assert out, "deploy_mode should return a status dict"
    leftovers = os.listdir(dx12_dir)
    assert leftovers == [], \
        "sister x64_dx12 dir must stay untouched, found: %s" % leftovers
    print("  deploy_mode dx11 leaves bin\\x64_dx12 untouched [PASS]")


print("\n=== Test E: launch env carries no NR_LAYER_HUDMASK (auto_mask on/off) ===")
with tempfile.TemporaryDirectory() as td:
    dummy = os.path.join(td, "game.exe")
    with open(dummy, "w") as f:
        f.write("dummy")
    hudmask_cfg = os.path.join(tempfile.gettempdir(), "m13_hudmask.cfg")
    try:
        os.remove(hudmask_cfg)
    except OSError:
        pass
    for auto_mask in (False, True):
        captured = {}

        def fake_launch(exe_path, env_extra=None):
            captured["env"] = dict(env_extra or {})
            return True, "launched pid 1"

        class _Cfg(dict):
            def get_phase30(self, exe=None):
                return {"intensity": 1.0, "local_tone": 1.0,
                        "local_structure": 1.0, "skin_structure": -1.0,
                        "auto_mask": auto_mask, "style_index": 0,
                        "color_strength": 1.0}

        with patch("m13.gamelaunch._launch", side_effect=fake_launch), \
             patch("m13.gamelaunch._flag_set"), \
             patch("m13.gamelaunch.freeze_set"):
            ok, msg = gamelaunch.launch_game(dummy, "dx11", cfg=_Cfg())
            assert ok, "launch_game failed: %s" % msg
            env = captured["env"]
            assert "NR_LAYER_HUDMASK" not in env, \
                "NR_LAYER_HUDMASK must not be set (auto_mask=%s)" % auto_mask
            assert env.get("NR_LAYER_UI_MASK") == "1", "static reuse env must stay on"
            assert not os.path.exists(hudmask_cfg), \
                "auto_mask=%s must not create %s" % (auto_mask, hudmask_cfg)
            print("  no NR_LAYER_HUDMASK and no m13_hudmask.cfg (auto_mask=%s) [PASS]" % auto_mask)


print("\n=== Test F: legacy hudmask games key + stale m13_hudmask.cfg cleanup ===")
with tempfile.TemporaryDirectory() as td:
    dummy = os.path.join(td, "game.exe")
    with open(dummy, "w") as f:
        f.write("dummy")
    fake_temp = os.path.join(td, "faketemp")
    os.makedirs(fake_temp)
    stale_cfg = os.path.join(fake_temp, "m13_hudmask.cfg")
    with open(stale_cfg, "w") as f:
        f.write("rect 0 0 0.1 0.1\n")
    cfg_file = os.path.join(td, "test_cfg.json")
    cfg = Config(cfg_file)
    cfg.set("games", {
        dummy: {"mode": "vulkan", "arch": "x64", "name": "TestX64",
                "hudmask": [{"shape": "rect", "x": 0, "y": 0, "w": 0.1, "h": 0.1}]},
    })
    with patch("m13.controller.tempfile.gettempdir", return_value=fake_temp):
        ctl = M13Controller(cfg)
    try:
        games = cfg.get("games")
        assert dummy in games, "existing game entry should survive migration"
        assert "hudmask" not in games[dummy], "hudmask key should be stripped"
        assert not os.path.exists(stale_cfg), "stale m13_hudmask.cfg should be deleted on init"
        print("  hudmask games key stripped and stale cfg deleted [PASS]")
    finally:
        ctl.shutdown()


print("\nALL LAUNCH PATH TESTS PASSED!")
