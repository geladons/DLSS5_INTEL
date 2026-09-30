# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# test_env.py - launch-env and config-dedupe coverage:
#   * gamelaunch.engine_env sets DLSS5_WEIGHTS_PATH / NR_LAYER_WEIGHTS and
#     DLSS5_SHADER_DIR / NR_LAYER_SHADERS for every injection mode, always
#     pointing the shader keys at a dir that actually holds gemm.spv.
#   * launch_game passes those keys through to the spawned process env.
#   * M13Controller seeds the same keys into os.environ.
#   * config._dedupe_games merges slash/case-duplicate game keys.
# Fully offline: temp dirs + patched spawns/deploy paths only.
# ============================================================================
import json
import os
import sys
import tempfile
from unittest.mock import patch

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.config import Config, _dedupe_games
from m13 import deploy as deploy_mod
from m13 import gamelaunch
from m13 import paths

ENGINE_KEYS = ("DLSS5_WEIGHTS_PATH", "NR_LAYER_WEIGHTS",
               "DLSS5_SHADER_DIR", "NR_LAYER_SHADERS")


def _w(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


print("=== Test 1: _dedupe_games merges slash-duplicate keys ===")
games = {
    "D:/GTA SA/gta_sa.exe": {"mode": "dx9", "arch": "x86",
                             "name": "gta_sa"},
    "D:\\GTA SA\\gta_sa.exe": {"mode": "dx12", "arch": "x86",
                              "name": "gta_sa-dup"},
    "D:/GTAV/GTA5_Enhanced.exe": {"mode": "dx12", "arch": "x64",
                                  "name": "GTA5"},
    "D:\\GTAV\\GTA5_Enhanced.exe": {"mode": "dx12", "arch": "x64",
                                   "name": "GTA5",
                                   "launch_exe": "D:\\GTAV\\PlayGTAV.exe"},
    "D:\\Solo\\solo.exe": {"mode": "dx11", "arch": "x64", "name": "solo"},
}
out, dropped = _dedupe_games(games)
assert len(out) == 3, "expected 3 merged entries, got %d: %s" % (len(out), out)
sa = out[os.path.normpath("D:/GTA SA/gta_sa.exe")]
assert sa["mode"] == "dx9", "no launch_exe anywhere -> FIRST wins: %s" % sa
g5 = out[os.path.normpath("D:/GTAV/GTA5_Enhanced.exe")]
assert g5.get("launch_exe") == os.path.normpath("D:/GTAV/PlayGTAV.exe") \
    or g5.get("launch_exe") == "D:\\GTAV\\PlayGTAV.exe", \
    "record with launch_exe must win: %s" % g5
assert len(dropped) == 2, dropped
assert _dedupe_games({}) == ({}, []), "empty games -> empty result"
assert _dedupe_games(None) == ({}, []), "None games -> empty result"
print("  launch_exe wins, else first-kept; dropped=%d [PASS]" % len(dropped))


print("\n=== Test 2: Config.load dedupes+normalizes games from disk ===")
with tempfile.TemporaryDirectory() as td:
    cfg_file = os.path.join(td, "config.json")
    with open(cfg_file, "w", encoding="ascii") as f:
        json.dump({"games": games}, f)
    cfg = Config(cfg_file)
    g = cfg.get("games")
    assert len(g) == 3, "config games not deduped: %s" % g
    assert os.path.normpath("D:/GTA SA/gta_sa.exe") in g
    assert g[os.path.normpath("D:/GTAV/GTA5_Enhanced.exe")].get("launch_exe"), \
        "launch_exe record must survive dedupe"
    print("  Config.load stored deduped normpath keys [PASS]")


print("\n=== Test 3: engine_env sets all 4 keys, per-mode shader dirs ===")
with tempfile.TemporaryDirectory() as td:
    m12_dir = os.path.join(td, "runtime", "m12")
    layer_dir = os.path.join(td, "runtime", "layer", "x64")
    wdir = os.path.join(td, "weights")
    _w(os.path.join(m12_dir, "gemm.spv"), b"spv")
    _w(os.path.join(layer_dir, "gemm.spv"), b"spv")
    weights = _w(os.path.join(wdir, "dlssnr-logical.safetensors"), b"w")
    m12_proxy = _w(os.path.join(m12_dir, "m12_dxgi.dll"), b"dll")
    manifest = _w(os.path.join(layer_dir, "VkLayer_dlssnr_win.json"), b"j")
    cfg = {"weights_path": weights}
    fake_find = lambda k: wdir if k == "weights" else os.path.join(td, k)
    with patch.object(deploy_mod, "M12_PROXY", m12_proxy), \
         patch.object(deploy_mod, "MANIFEST_X64", manifest), \
         patch.object(paths, "find", side_effect=fake_find):
        for mode, want in (("dx12", m12_dir), ("dx11", layer_dir),
                           ("vulkan", layer_dir), ("dx9", layer_dir),
                           (None, m12_dir)):
            ee = gamelaunch.engine_env(cfg, mode, "x64")
            for k in ENGINE_KEYS:
                assert k in ee, "%s missing for mode=%s" % (k, mode)
            assert ee["DLSS5_WEIGHTS_PATH"] == weights
            assert ee["NR_LAYER_WEIGHTS"] == weights
            assert ee["DLSS5_SHADER_DIR"] == want, \
                "mode=%s -> %s, want %s" % (mode, ee["DLSS5_SHADER_DIR"], want)
            assert ee["NR_LAYER_SHADERS"] == want
            assert os.path.exists(
                os.path.join(ee["DLSS5_SHADER_DIR"], "gemm.spv"))
        print("  4 keys set; every mode -> dir containing gemm.spv [PASS]")

        # No gemm.spv anywhere -> shader keys must stay unset.
        empty = os.path.join(td, "empty")
        _w(os.path.join(empty, "VkLayer_dlssnr_win.json"), b"j")
        _w(os.path.join(empty, "m12_dxgi.dll"), b"dll")
        with patch.object(deploy_mod, "M12_PROXY",
                          os.path.join(empty, "m12_dxgi.dll")), \
             patch.object(deploy_mod, "MANIFEST_X64",
                          os.path.join(empty, "VkLayer_dlssnr_win.json")):
            ee2 = gamelaunch.engine_env(cfg, "dx12", "x64")
            assert "DLSS5_SHADER_DIR" not in ee2
            assert "NR_LAYER_SHADERS" not in ee2
            assert ee2["DLSS5_WEIGHTS_PATH"] == weights
            print("  dirs without gemm.spv -> shader keys skipped [PASS]")

        # No cfg weights_path -> fallback = first *.safetensors in the
        # bundle weights dir.
        ee3 = gamelaunch.engine_env({}, "dx11", "x64")
        assert ee3["DLSS5_WEIGHTS_PATH"] == weights, ee3
        print("  missing weights_path -> weights-dir fallback [PASS]")


print("\n=== Test 4: launch_game env carries the engine keys ===")
with tempfile.TemporaryDirectory() as td:
    m12_dir = os.path.join(td, "m12")
    layer_dir = os.path.join(td, "layer64")
    _w(os.path.join(m12_dir, "gemm.spv"), b"spv")
    _w(os.path.join(layer_dir, "gemm.spv"), b"spv")
    weights = _w(os.path.join(td, "w.safetensors"), b"w")
    exe = _w(os.path.join(td, "game.exe"), b"e")
    captured = {}

    def fake_launch(exe_path, env_extra=None):
        captured["env"] = dict(env_extra or {})
        return True, "launched pid 1"

    class _Cfg(dict):
        pass                      # dict.get covers cfg.get

    for mode, want in (("dx12", m12_dir), ("dx11", layer_dir),
                       ("dx9", layer_dir), ("vulkan", layer_dir)):
        with patch("m13.gamelaunch._launch", side_effect=fake_launch), \
             patch("m13.gamelaunch._flag_set"), \
             patch("m13.gamelaunch.freeze_set"), \
             patch.object(deploy_mod, "M12_PROXY",
                          os.path.join(m12_dir, "m12_dxgi.dll")), \
             patch.object(deploy_mod, "MANIFEST_X64",
                          os.path.join(layer_dir, "VkLayer_dlssnr_win.json")):
            ok, msg = gamelaunch.launch_game(
                exe, mode, cfg=_Cfg(weights_path=weights))
            assert ok, "launch_game failed: %s" % msg
            env = captured["env"]
            for k in ENGINE_KEYS:
                assert k in env, "%s missing in launch env (mode=%s)" % (
                    k, mode)
            assert env["DLSS5_WEIGHTS_PATH"] == weights
            assert env["NR_LAYER_WEIGHTS"] == weights
            assert env["DLSS5_SHADER_DIR"] == want, (mode, env)
            assert env["NR_LAYER_SHADERS"] == want
            assert os.path.exists(
                os.path.join(env["DLSS5_SHADER_DIR"], "gemm.spv"))
        print("  mode=%s launch env ok [PASS]" % mode)


print("\n=== Test 5: M13Controller seeds os.environ ===")
with tempfile.TemporaryDirectory() as td:
    from m13.controller import M13Controller
    m12_dir = os.path.join(td, "m12")
    _w(os.path.join(m12_dir, "gemm.spv"), b"spv")
    _w(os.path.join(m12_dir, "m12_dxgi.dll"), b"dll")
    weights = _w(os.path.join(td, "w.safetensors"), b"w")
    cfg = Config(os.path.join(td, "cfg.json"))
    cfg.set("weights_path", weights)
    saved = {k: os.environ.get(k) for k in ENGINE_KEYS}
    try:
        with patch.object(deploy_mod, "M12_PROXY",
                          os.path.join(m12_dir, "m12_dxgi.dll")), \
             patch.object(deploy_mod, "MANIFEST_X64",
                          os.path.join(m12_dir, "VkLayer_dlssnr_win.json")):
            ctl = M13Controller(cfg)
        try:
            assert os.environ["DLSS5_WEIGHTS_PATH"] == weights
            assert os.environ["NR_LAYER_WEIGHTS"] == weights
            assert os.environ["DLSS5_SHADER_DIR"] == m12_dir
            assert os.environ["NR_LAYER_SHADERS"] == m12_dir
        finally:
            ctl.shutdown()
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    print("  os.environ seeded with weights+shader keys [PASS]")


print("\nALL ENV/DEDUPE TESTS PASSED!")
