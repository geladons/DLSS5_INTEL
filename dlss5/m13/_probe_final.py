# _probe_final.py - Task F verification against the real ControlOverlay.
# Measures GetForegroundWindow across show() and a real click on the blend
# slider with overlay_focus=False / overlay_freeze=False.
# UNTRACKED helper - do not stage.
import ctypes
import os
import sys
import time
import tkinter as tk
from ctypes import wintypes

_M13 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _M13)

from m13.overlay import ControlOverlay, _force_foreground  # noqa: E402

_u32 = ctypes.windll.user32
_u32.GetForegroundWindow.restype = wintypes.HWND
_u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]


class _INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("dx", ctypes.c_long),
                ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


def fg():
    return int(_u32.GetForegroundWindow() or 0)


class Cfg(dict):
    def set(self, k, v):
        self[k] = v

    def get_phase30(self):
        return {}


class StubCtl:
    def __init__(self):
        self.cfg = Cfg(scale=0.5, frame_gen=False, sharpness=0.8,
                       overlay_focus=False, overlay_freeze=False,
                       overlay_hotkey="control+alt+g")

    def snapshot(self):
        return {}

    def submit(self, *a, **k):
        pass

    def processing_paused(self):
        return None

    def active_game_pid(self):
        return None

    def freeze_supported(self):
        return False


def pump(root, ms):
    end = time.time() + ms / 1000.0
    while time.time() < end:
        try:
            root.update_idletasks()
            root.update()
        except tk.TclError:
            pass
        time.sleep(0.01)


def main():
    root = tk.Tk()
    root.withdraw()
    game = tk.Toplevel(root)
    game.title("probe-game")
    game.geometry("320x200+40+40")
    game.deiconify()
    game_frame = int(game.wm_frame(), 16)
    _force_foreground(game_frame)
    pump(root, 300)
    print("game hwnd=%s fg=%s" % (hex(game_frame), hex(fg())), flush=True)

    logs = []
    ov = ControlOverlay(root, StubCtl(), on_log=logs.append)
    pump(root, 200)

    fg_before = fg()
    ov.show()
    pump(root, 300)
    fg_after_show = fg()
    ov_hwnd = int(ov.win.wm_frame(), 16)
    print("show: fg %s -> %s  overlay=%s steal=%s focus_mode=%s" % (
        hex(fg_before), hex(fg_after_show), hex(ov_hwnd),
        fg_after_show == ov_hwnd, ov._focus_mode), flush=True)

    # real click on the blend slider
    ov.win.update_idletasks()
    x = ov.blend_scale.winfo_rootx() + ov.blend_scale.winfo_width() // 2
    y = ov.blend_scale.winfo_rooty() + ov.blend_scale.winfo_height() // 2
    _u32.SetCursorPos(x, y)
    time.sleep(0.1)
    dn = _INPUT(0, 0, 0, 0, 0x0002, 0, None)
    up = _INPUT(0, 0, 0, 0, 0x0004, 0, None)
    _u32.SendInput(1, ctypes.byref(dn), ctypes.sizeof(dn))
    time.sleep(0.15)
    _u32.SendInput(1, ctypes.byref(up), ctypes.sizeof(up))
    pump(root, 400)
    fg_after_click = fg()
    print("click@(%d,%d): fg %s -> %s steal=%s" % (
        x, y, hex(fg_after_show), hex(fg_after_click),
        fg_after_click == ov_hwnd), flush=True)

    # takefocus check over the whole tree
    bad = []

    def _tree(w):
        out = [w]
        for ch in w.winfo_children():
            out.extend(_tree(ch))
        return out

    for w in _tree(ov.win):
        try:
            if str(w.cget("takefocus")) != "0":
                bad.append(str(w))
        except tk.TclError:
            pass
    print("takefocus offenders: %s" % (bad or "none"), flush=True)

    ov.hide()
    pump(root, 200)
    print("hide: fg=%s" % hex(fg()), flush=True)
    print("--- on_log lines ---", flush=True)
    for m in logs:
        print("  " + m, flush=True)
    print("PROBE DONE", flush=True)
    root.destroy()


if __name__ == "__main__":
    main()
