# click-delivery check v3: identify the child window under the cursor and
# test whether clicks reach Tk at all on a NOACTIVATE overlay.
import ctypes
import os
import sys
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import ttk

_M13 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _M13)
from m13.overlay import _set_noactivate, _force_foreground  # noqa: E402

_u32 = ctypes.windll.user32
_u32.GetForegroundWindow.restype = wintypes.HWND
_u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
_u32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
_u32.WindowFromPoint.argtypes = [wintypes.POINT]
_u32.WindowFromPoint.restype = wintypes.HWND
_u32.GetAncestor.argtypes = [wintypes.HWND, ctypes.c_uint]
_u32.GetAncestor.restype = wintypes.HWND
_u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint]
_u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_k32 = ctypes.windll.kernel32


class _INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("dx", ctypes.c_long),
                ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


def fg():
    return int(_u32.GetForegroundWindow() or 0)


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    _u32.GetClassNameW(h, b, 256)
    return b.value


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

    ov = tk.Toplevel(root)
    ov.overrideredirect(True)
    ov.attributes("-topmost", True)
    ov.geometry("300x160+420+260")
    hits = {"ov": 0, "btn": 0, "scale": 0, "all": 0}
    btn = tk.Button(ov, text="B", command=lambda: hits.__setitem__(
        "btn", hits["btn"] + 1))
    btn.pack(pady=8)
    scale = ttk.Scale(ov, from_=0.0, to=1.0, command=lambda _v: hits.__setitem__(
        "scale", hits["scale"] + 1))
    scale.pack(fill="x", padx=24, pady=20)
    ov.bind("<Button-1>", lambda _e: hits.__setitem__("ov", hits["ov"] + 1))
    root.bind_all("<Button-1>", lambda _e: hits.__setitem__(
        "all", hits["all"] + 1))
    ov.deiconify()
    root.update()
    ov_frame = int(ov.wm_frame(), 16)
    _set_noactivate(ov_frame, True)
    _force_foreground(int(game.wm_frame(), 16))
    pump(root, 300)
    _u32.SetWindowPos(ov_frame, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
    pump(root, 150)

    for name, wgt in (("button", btn), ("scale", scale)):
        ov.update_idletasks()
        x = wgt.winfo_rootx() + max(4, wgt.winfo_width() // 2)
        y = wgt.winfo_rooty() + wgt.winfo_height() // 2
        _u32.SetCursorPos(x, y)
        time.sleep(0.1)
        pt = wintypes.POINT()
        _u32.GetCursorPos(ctypes.byref(pt))
        under = int(_u32.WindowFromPoint(pt) or 0)
        print("%s click@(%d,%d) under=%s cls=%r ours=%s" % (
            name, x, y, hex(under), cls(under),
            int(_u32.GetAncestor(under, 2) or 0) == ov_frame), flush=True)
        print("  winfo_containing=%s" % (ov.winfo_containing(x, y),), flush=True)
        before = fg()
        down = _INPUT(0, 0, 0, 0, 0x0002, 0, None)
        up = _INPUT(0, 0, 0, 0, 0x0004, 0, None)
        _u32.SendInput(1, ctypes.byref(down), ctypes.sizeof(down))
        time.sleep(0.15)
        _u32.SendInput(1, ctypes.byref(up), ctypes.sizeof(up))
        pump(root, 400)
        print("  hits=%s fg %s -> %s" % (hits, hex(before), hex(fg())),
              flush=True)

    print("DONE", flush=True)
    root.destroy()


if __name__ == "__main__":
    main()
