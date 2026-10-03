"""Games-tab extras row: launch_args + framegen + window must survive every
write path (focus-out save, register-on-launch, Enable re-register)."""
import os, tempfile, tkinter as tk
from tkinter import ttk
import pytest

from m13 import config, ui_games

EXE = r"D:\games\Foo.exe"


class _Ctl:
    def __init__(self, cfg):
        self.cfg = cfg
        self.calls = []
    def submit(self, fn, *a):
        self.calls.append(fn.__name__)
    def launch_game_async(self, exe, register=None):
        self.calls.append(("launch", register))
    def add_game(self, exe, mode=None, arch=None, name=None, launch_exe=None):
        g = dict(self.cfg.get("games") or {})
        g[exe] = {"mode": mode or "dx9", "arch": "x86", "name": "X"}
        self.cfg.set("games", g)
        return True, "ok"
    def game_deploy(self, exe):
        return True, "dep"


@pytest.fixture
def tab(monkeypatch, tmp_path):
    monkeypatch.setattr(config, "CONFIG_PATH", str(tmp_path / "c.json"))
    root = tk.Tk()
    root.withdraw()
    ui = type("U", (), {"ctl": _Ctl(config.Config(str(tmp_path / "c.json"))),
                        "log": lambda *a: None})()
    t = ui_games.GamesTab(ui, ttk.Notebook(root))
    yield t, ui
    root.destroy()


def test_extras_survive_every_path(tab):
    t, ui = tab
    t.select(EXE)
    assert t.args_var.get() == "" and t.fg_var.get() == "auto"
    assert not t.window_var.get()

    t.args_var.set("-windowed -w 1280")
    t.fg_var.set("off")
    t._launch()
    reg = ui.ctl.calls[-1][1]
    assert reg.get("launch_args") == "-windowed -w 1280"
    assert reg.get("framegen") is False

    ui.ctl.cfg.set("games", {EXE: {"mode": "dx9", "arch": "x86", "name": "Foo",
                                   "launch_args": "-x", "framegen": True,
                                   "window": True}})
    t.select(EXE)
    assert (t.args_var.get(), t.fg_var.get(), t.window_var.get()) == \
           ("-x", "on", True)

    t.args_var.set("-y")
    t._save_extras()
    assert ui.ctl.cfg.get("games")[EXE]["launch_args"] == "-y"
    t.fg_var.set("auto")
    t._save_extras()
    assert "framegen" not in ui.ctl.cfg.get("games")[EXE]

    ui.ctl.cfg.set("games", {})
    t.select(EXE)
    t.args_var.set("-z")
    t.fg_var.set("off")
    t.window_var.set(True)
    t._enable_worker(EXE, "dx9", None, "-z", False, True)
    m = ui.ctl.cfg.get("games")[EXE]
    assert m["launch_args"] == "-z" and m["framegen"] is False
    assert m["window"] is True
