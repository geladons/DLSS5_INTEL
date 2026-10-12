"""Games-tab extras row: framegen/temporal persist via _save_extras; launch
options flow through apply_launch_opts (dialog OK) into the meta of a
registered game or pending_opts until Enable/Play carries them."""
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
    assert t.fg_var.get() == "default"

    # Unregistered card: the dialog's OK parks the opts; Play carries them
    # into the register dict together with the framegen pick.
    t.apply_launch_opts(EXE, {"window": True, "window_w": 1280,
                              "window_h": 720, "window_template": "gtaiv",
                              "launch_args": "-nolauncher"})
    t.fg_var.set("off")
    t._launch()
    reg = ui.ctl.calls[-1][1]
    assert reg["window"] is True and reg["window_template"] == "gtaiv"
    assert reg["launch_args"] == "-nolauncher"
    assert reg.get("framegen") is False
    assert EXE not in t.pending_opts           # consumed by the launch

    # Registered game: OK writes cfg directly; empty extra args remove the
    # launch_args key.
    ui.ctl.cfg.set("games", {EXE: {"mode": "dx9", "arch": "x86", "name": "Foo",
                                   "framegen": True,
                                   "launch_args": "-x"}})
    t.select(EXE)
    assert t.fg_var.get() == "on"
    t.apply_launch_opts(EXE, {"window": False, "window_w": 800,
                              "window_h": 600, "window_template": "none",
                              "launch_args": ""})
    m = ui.ctl.cfg.get("games")[EXE]
    assert m["window"] is False and m["window_template"] == "none"
    assert m["window_w"] == 800 and m["window_h"] == 600
    assert "launch_args" not in m

    t.fg_var.set("default")
    t._save_extras()
    assert "framegen" not in ui.ctl.cfg.get("games")[EXE]

    # Unregistered again: pending opts survive into Enable's fresh meta.
    ui.ctl.cfg.set("games", {})
    t.select(EXE)
    t.apply_launch_opts(EXE, {"window": True, "window_w": 800,
                              "window_h": 600, "window_template": "source",
                              "launch_args": "-z"})
    t.fg_var.set("off")
    t._enable_worker(EXE, "dx9", None, False)
    m = ui.ctl.cfg.get("games")[EXE]
    assert m["launch_args"] == "-z" and m["framegen"] is False
    assert m["window"] is True and m["window_template"] == "source"
    assert m["window_w"] == 800 and m["window_h"] == 600
    assert EXE not in t.pending_opts           # consumed by the enable
