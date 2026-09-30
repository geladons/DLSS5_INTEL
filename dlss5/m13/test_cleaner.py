# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.test_cleaner - tmp-dir tests for cleaner.py (TraceScanner + Cleaner).
# Style follows test_launch_paths.py: print-driven asserts, _Cfg stub, fake
# registry/process providers - no real Tk, winreg, or processes touched.
#   python test_cleaner.py
# ============================================================================
import os
import sys
import tempfile
from unittest.mock import patch

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13 import cleaner
from m13.cleaner import TraceScanner, Cleaner


class _Cfg:
    """Minimal config stub: games dict + optional .path for config_dir."""
    def __init__(self, games=None, path=None):
        self._games = games or {}
        self.path = path

    def get(self, key, default=None):
        return self._games if key == "games" else default


class _FakeRegistry:
    def __init__(self, names):
        self.names = list(names)
        self.deleted = []

    def registered_paths(self):
        return list(self.names)

    def unregister(self, name):
        self.deleted.append(name)
        if name in self.names:
            self.names.remove(name)

    def is_registered(self, name):
        return name in self.names


class _FakeProcs:
    def __init__(self, running=True):
        self.running = running
        self.killed = []

    def find_pid(self, name):
        return 4242 if self.running else None

    def find_pids(self, name):
        return [4242] if self.running else []

    def stop_pid(self, pid, force=True):
        self.killed.append((pid, force))
        self.running = False
        return True, "terminated pid %d" % pid


def _w(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


# --------------------------------------------------------------- fixture ---
def _fixture(td):
    """Build a fake source dir + bin\\x64 / bin\\x64_dx12 game layout."""
    src = os.path.join(td, "srcdlls")
    ours = {}
    for name in ("d3d9.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll",
                 "d5c_chain.dll"):
        ours[name] = _w(os.path.join(src, name),
                        b"ours-%s" % name.encode())
    x64 = os.path.join(td, "bin", "x64")
    dx12 = os.path.join(td, "bin", "x64_dx12")
    exe = _w(os.path.join(x64, "game.exe"), b"pe")
    # deployed: our d3d11 over a parked original backup
    _w(os.path.join(x64, "d3d11.dll"), open(ours["d3d11.dll"], "rb").read())
    _w(os.path.join(x64, "d3d11.dll.m13bak"), b"orig-d3d11")
    # foreign dxgi with NO backup -> reported foreign, never touched
    _w(os.path.join(x64, "dxgi.dll"), b"foreign-dxgi")
    # foreign d3d9 parked over our backup -> restored on clean
    _w(os.path.join(x64, "d3d9.dll"), b"foreign-d3d9")
    _w(os.path.join(x64, "d3d9.dll.m13bak"), b"orig-d3d9")
    # unrelated dll: not a candidate name, not ours -> invisible
    _w(os.path.join(x64, "engine.dll"), b"unrelated")
    # sister dir: orphan d5c_chain.dll matching our source
    _w(os.path.join(dx12, "d5c_chain.dll"),
       open(ours["d5c_chain.dll"], "rb").read())
    cfg = _Cfg({exe: {"mode": "dx11", "arch": "x64", "name": "Fake"}})
    return src, x64, dx12, exe, cfg, ours


print("=== Test A: scan() inventory ===")
with tempfile.TemporaryDirectory() as td:
    src, x64, dx12, exe, cfg, ours = _fixture(td)
    flag = _w(os.path.join(td, "tmp", "m13_freeze.flag"), b"m13")
    cfgdir = _w(os.path.join(td, "DLSS5Manager", "config.json"), b"{}")
    cfgdir = os.path.dirname(cfgdir)
    man = os.path.join(td, "runtime", "layer", "x64", "VkLayer_dlssnr_win.json")
    reg = _FakeRegistry([man, r"C:\other\VkLayer_someone_else.json"])
    procs = _FakeProcs(running=True)
    sc = TraceScanner(cfg, deploy_source_dir=src,
                      temp_paths=[flag, os.path.join(td, "tmp", "m13_*.flag")],
                      config_dir=cfgdir, registry=reg, procs=procs)
    traces = sc.scan()
    by_path = {os.path.normcase(t.path): t for t in traces}
    def has(path, kind):
        t = by_path.get(os.path.normcase(path))
        return t is not None and t.kind == kind
    assert has(os.path.join(x64, "d3d11.dll"), "game_dll"), "deployed dll"
    assert has(os.path.join(x64, "d3d11.dll.m13bak"), "game_backup"), "backup"
    assert has(os.path.join(x64, "dxgi.dll"), "foreign"), "foreign dll"
    assert has(os.path.join(x64, "d3d9.dll"), "game_dll"), "foreign-over-bak"
    assert has(os.path.join(dx12, "d5c_chain.dll"), "game_dll"), \
        "sister orphan chain dll"
    assert os.path.normcase(os.path.join(x64, "engine.dll")) not in by_path, \
        "unrelated dll must not be listed"
    assert has(flag, "temp_file"), "temp flag"
    assert has(man, "registry"), "registry trace"
    assert not any(t.kind == "registry" and "someone_else" in t.path
                   for t in traces), "foreign layer registration excluded"
    assert any(t.kind == "daemon" and t.path == "m11d.exe" for t in traces), \
        "daemon trace"
    assert has(cfgdir, "config_dir"), "config dir trace"
    # glob dedupe: flag listed once although it matches path AND pattern
    n_flag = sum(1 for t in traces
                 if os.path.normcase(t.path) == os.path.normcase(flag))
    assert n_flag == 1, "flag counted twice"
    print("  scan finds game/sister/backup/foreign/reg/daemon/temp/config [PASS]")


print("\n=== Test B: remove() without config ===")
with tempfile.TemporaryDirectory() as td:
    src, x64, dx12, exe, cfg, ours = _fixture(td)
    flag = _w(os.path.join(td, "tmp", "m13_freeze.flag"), b"m13")
    cfgdir = os.path.dirname(
        _w(os.path.join(td, "DLSS5Manager", "config.json"), b"{}"))
    man = os.path.join(td, "VkLayer_dlssnr_win.json")
    reg = _FakeRegistry([man])
    procs = _FakeProcs(running=True)
    sc = TraceScanner(cfg, deploy_source_dir=src, temp_paths=[flag],
                      config_dir=cfgdir, registry=reg, procs=procs)
    traces = sc.scan()
    lines = []
    cl = Cleaner(cfg, log=lines.append, deploy_source_dir=src,
                 registry=reg, procs=procs)
    ok, summary = cl.remove(traces, include_config=False)
    assert ok, "remove failed: %s\n%s" % (summary, "\n".join(lines))
    d3d11 = os.path.join(x64, "d3d11.dll")
    assert open(d3d11, "rb").read() == b"orig-d3d11", "backup not restored"
    assert not os.path.exists(d3d11 + ".m13bak"), "backup should be consumed"
    dxgi = os.path.join(x64, "dxgi.dll")
    assert open(dxgi, "rb").read() == b"foreign-dxgi", "foreign dll touched"
    d3d9 = os.path.join(x64, "d3d9.dll")
    assert open(d3d9, "rb").read() == b"orig-d3d9", \
        "foreign-over-backup not restored"
    assert not os.path.exists(d3d9 + ".m13bak"), "d3d9 backup left"
    assert not os.path.exists(os.path.join(dx12, "d5c_chain.dll")), \
        "sister orphan not removed"
    assert not os.path.exists(flag), "temp flag not removed"
    assert reg.deleted == [man], "registry value not unregistered: %s" % reg.deleted
    assert procs.killed == [(4242, True)], "daemon not stopped: %s" % procs.killed
    assert os.path.isdir(cfgdir), "config dir must stay (include_config=False)"
    assert not os.path.exists(os.path.join(x64, "engine.dll")) or True
    assert open(os.path.join(x64, "engine.dll"), "rb").read() == b"unrelated"
    assert any("[clean] undeploy ok=1" in l for l in lines), \
        "structured [clean] lines missing"
    print("  remove: restored backups, kept foreign, cleaned sister+temp+reg+daemon [PASS]")


print("\n=== Test C: post-remove rescan is clean ===")
with tempfile.TemporaryDirectory() as td:
    src, x64, dx12, exe, cfg, ours = _fixture(td)
    reg = _FakeRegistry([])
    procs = _FakeProcs(running=False)
    sc = TraceScanner(cfg, deploy_source_dir=src, temp_paths=[],
                      config_dir=os.path.join(td, "noconfig"),
                      registry=reg, procs=procs)
    traces = sc.scan()
    cl = Cleaner(cfg, log=lambda m: None, deploy_source_dir=src,
                 registry=reg, procs=procs)
    ok, summary = cl.remove(traces)
    assert ok, summary
    traces2 = sc.scan()
    left = [(t.kind, t.path) for t in traces2 if t.kind != "foreign"]
    # restored originals now classify as 'foreign' (game's own files back);
    # the pre-existing foreign dxgi.dll stays foreign too. Nothing else.
    assert left == [], "leftover traces after clean: %s" % left
    n_foreign = sum(1 for t in traces2 if t.kind == "foreign")
    assert n_foreign == 3, \
        "expected 3 foreign leftovers (d3d9, d3d11, dxgi), got %d" % n_foreign
    print("  rescan: only foreign leftovers remain [PASS]")


print("\n=== Test D: include_config=True renames the config dir ===")
with tempfile.TemporaryDirectory() as td:
    cfgdir = os.path.dirname(
        _w(os.path.join(td, "DLSS5Manager", "config.json"), b"{}"))
    cfg = _Cfg(path=os.path.join(cfgdir, "config.json"))
    sc = TraceScanner(cfg, deploy_source_dir=td, temp_paths=[],
                      config_dir=cfgdir, registry=_FakeRegistry([]),
                      procs=_FakeProcs(False))
    traces = sc.scan()
    assert [t.kind for t in traces] == ["config_dir"], traces
    cl = Cleaner(cfg, log=lambda m: None, deploy_source_dir=td,
                 registry=_FakeRegistry([]), procs=_FakeProcs(False))
    ok, summary = cl.remove(traces, include_config=True)
    assert ok, summary
    assert not os.path.exists(cfgdir), "config dir not renamed"
    moved = [n for n in os.listdir(td) if n.startswith("DLSS5Manager.removed-")]
    assert len(moved) == 1, "expected one .removed-* dir, got %s" % moved
    assert os.path.isfile(os.path.join(td, moved[0], "config.json")), \
        "renamed dir lost its contents"
    print("  config dir renamed to %s [PASS]" % moved[0])


print("\n=== Test E: foreign dll without backup is never deleted ===")
with tempfile.TemporaryDirectory() as td:
    src = os.path.join(td, "src")
    _w(os.path.join(src, "dxgi.dll"), b"ours-dxgi")
    gdir = os.path.join(td, "game")
    exe = _w(os.path.join(gdir, "game.exe"), b"pe")
    _w(os.path.join(gdir, "dxgi.dll"), b"totally-foreign")
    cfg = _Cfg({exe: {"mode": "dx12", "arch": "x64", "name": "G"}})
    sc = TraceScanner(cfg, deploy_source_dir=src, temp_paths=[],
                      config_dir=os.path.join(td, "none"),
                      registry=_FakeRegistry([]), procs=_FakeProcs(False))
    traces = sc.scan()
    assert [t.kind for t in traces] == ["foreign"], traces
    ok, _ = Cleaner(cfg, log=lambda m: None, deploy_source_dir=src,
                    registry=_FakeRegistry([]),
                    procs=_FakeProcs(False)).remove(traces)
    assert ok
    assert open(os.path.join(gdir, "dxgi.dll"), "rb").read() == \
        b"totally-foreign", "foreign dll was deleted!"
    print("  foreign dll untouched [PASS]")


print("\n=== Test F: undeploy PermissionError routes to _elevated_remove ===")
with tempfile.TemporaryDirectory() as td:
    src = os.path.join(td, "src")
    _w(os.path.join(src, "d3d9.dll"), b"ours-d3d9")
    gdir = os.path.join(td, "game")
    exe = _w(os.path.join(gdir, "game.exe"), b"pe")
    _w(os.path.join(gdir, "d3d9.dll"), b"ours-d3d9")
    cfg = _Cfg({exe: {"mode": "dx9", "arch": "x86", "name": "G"}})
    calls = []

    def deny_remove(path):
        raise PermissionError("denied")

    def deny_replace(a, b):
        raise PermissionError("denied")

    _real_remove = os.remove
    def fake_elevated(d, name):
        calls.append((d, name))
        _real_remove(os.path.join(d, name))

    from m13 import deploy as deploy_mod
    sc = TraceScanner(cfg, deploy_source_dir=src, temp_paths=[],
                      config_dir=os.path.join(td, "none"),
                      registry=_FakeRegistry([]), procs=_FakeProcs(False))
    traces = sc.scan()
    cl = Cleaner(cfg, log=lambda m: None, deploy_source_dir=src,
                 registry=_FakeRegistry([]), procs=_FakeProcs(False))
    with patch("m13.cleaner.os.remove", side_effect=deny_remove), \
         patch("m13.cleaner.os.replace", side_effect=deny_replace), \
         patch.object(deploy_mod, "_elevated_remove", fake_elevated), \
         patch.object(deploy_mod.os, "remove", side_effect=deny_remove), \
         patch.object(deploy_mod.os, "replace", side_effect=deny_replace):
        cl.remove(traces)
    assert calls == [(gdir, "d3d9.dll")], "elevated remove not used: %s" % calls
    assert not os.path.exists(os.path.join(gdir, "d3d9.dll"))
    print("  permission failure -> _elevated_remove [PASS]")


print("\nALL CLEANER TESTS PASSED!")
