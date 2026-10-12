"""gamelaunch.build_launch_args / suggest_template: windowed templates,
legacy fallback, {screen_*} expansion, shlex quoting of launch_args."""
import os
import sys

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13 import gamelaunch


def test_window_off_keeps_extra_args():
    # Template fields present but window off -> no template flags at all;
    # extra args still pass through (they work windowed or fullscreen).
    meta = {"window": False, "window_template": "gtaiv",
            "window_w": 800, "window_h": 600,
            "launch_args": "-foo bar"}
    assert gamelaunch.build_launch_args(meta) == ["-foo", "bar"]


def test_gtaiv_template_resolution():
    meta = {"window": True, "window_template": "gtaiv",
            "window_w": 800, "window_h": 600}
    assert gamelaunch.build_launch_args(meta) == \
        ["-windowed", "-width", "800", "-height", "600"]


def test_legacy_meta_falls_back_to_generic():
    # window=True with empty args, written before window_template
    # existed -> the old generic pair.
    assert gamelaunch.build_launch_args({"window": True}) == \
        ["-windowed", "-window"]


def test_legacy_meta_with_args_behaves_as_custom():
    # Owner's live GTA IV entry: window=true + an explicit args string
    # and no template. The old contract was "explicit args win" - the
    # argv must come out byte-identical (no -windowed -window prefix).
    meta = {"window": True,
            "launch_args": "-windowed -width 800 -height 600"}
    assert gamelaunch.build_launch_args(meta) == \
        ["-windowed", "-width", "800", "-height", "600"]


def test_template_none_and_custom():
    assert gamelaunch.build_launch_args(
        {"window": True, "window_template": "none"}) == []
    # "custom" means no template flags, NOT a fallback to generic.
    assert gamelaunch.build_launch_args(
        {"window": True, "window_template": "custom",
         "launch_args": "-x"}) == ["-x"]


def test_extra_args_quoted_path():
    # posix=False keeps the quotes so Windows groups the spaced path
    # back into one argv entry.
    meta = {"window": False,
            "launch_args": '-config "C:\\My Games\\cfg file.ini"'}
    assert gamelaunch.build_launch_args(meta) == \
        ["-config", '"C:\\My Games\\cfg file.ini"']


def test_screen_placeholders_expand():
    meta = {"window": False,
            "launch_args": "-width {screen_w} -height {screen_h}"}
    assert gamelaunch.build_launch_args(meta, screen=(2560, 1440)) == \
        ["-width", "2560", "-height", "1440"]


def test_suggest_template():
    s = gamelaunch.suggest_template
    assert s(r"D:\games\GTAIV.exe") == "gtaiv"
    assert s(r"D:\games\EFLC.exe") == "gtaiv"
    assert s(r"D:\games\gta_sa.exe") == "none"
    assert s(r"D:\games\MetroExodus.exe") == "none"
    assert s(r"D:\games\Foo-Win64-Shipping.exe") == "unreal"
    assert s(r"D:\games\My Game\Binaries\Win64\Game.exe") == "unreal"
    assert s(r"D:\games\hl2.exe") == "source"
    assert s(r"D:\games\portal2.exe") == "source"
    assert s(r"D:\games\Foo.exe") == "generic"
