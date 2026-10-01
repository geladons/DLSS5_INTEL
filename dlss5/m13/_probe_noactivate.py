# ============================================================================
# _probe_noactivate.py - empirical probe for Task F.
# Measures whether each show/focus step transfers GetForegroundWindow()
# to a WS_EX_NOACTIVATE tkinter Toplevel, against a stand-in "game" window.
# Run detached; prints a table. UNTRACKED helper - do not stage.
# ============================================================================
import ctypes
import os
import subprocess
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
_u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_u32.ShowWindow.restype = wintypes.BOOL
_u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint]
_u32.SetWindowPos.restype = wintypes.BOOL
_u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
_u32.SetCursorPos.restype = wintypes.BOOL
_u32.IsWindowVisible.argtypes = [wintypes.HWND]
_u32.IsWindowVisible.restype = wintypes.BOOL

SW_SHOWNOACTIVATE = 4
HWND_TOPMOST = -1
SWP_SHOW_NOACTIVATE = 0x0002 | 0x0001 | 0x0010 | 0x0040  # NOMOVE|NOSIZE|NOACTIVATE|SHOWWINDOW
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


class _INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("dx", ctypes.c_long),
                ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


def fg():
    return int(_u32.GetForegroundWindow() or 0)


def fh(h):
    return "0x%08x" % h if h else "0x00000000"


def click(x, y):
    _u32.SetCursorPos(x, y)
    time.sleep(0.08)
    down = _INPUT(0, 0, 0, 0, MOUSEEVENTF_LEFTDOWN, 0, None)
    up = _INPUT(0, 0, 0, 0, MOUSEEVENTF_LEFTUP, 0, None)
    _u32.SendInput(1, ctypes.byref(down), ctypes.sizeof(down))
    time.sleep(0.15)
    _u32.SendInput(1, ctypes.byref(up), ctypes.sizeof(up))


rows = []


def report(step, before, after, ov_hwnd, note=""):
    stole = (after == ov_hwnd)
    rows.append((step, fh(before), fh(after), "STEAL" if stole else "ok", note))
    print("%-28s fg_before=%s fg_after=%s %s %s" % (
        step, fh(before), fh(after), "STEAL" if stole else "ok", note),
        flush=True)


def run_pass(game_hwnd, game_name, tkroot, ov, ov_hwnd, scale, clicked):
    print("\n==== pass: foreground holder = %s %s ====" % (game_name, fh(game_hwnd)),
          flush=True)

    def settle(ms=250):
        end = time.time() + ms / 1000.0
        while time.time() < end:
            try:
                tkroot.update_idletasks()
                tkroot.update()
            except tk.TclError:
                pass
            time.sleep(0.01)

    def game_front():
        _force_foreground(game_hwnd)
        settle(300)
        return fg()

    # (a) deiconify
    before = game_front()
    ov.deiconify()
    settle(300)
    report("(a) deiconify", before, fg(), ov_hwnd,
           "viewable=%d" % ov.winfo_viewable())

    # (b) -topmost + lift
    before = game_front()
    ov.attributes("-topmost", True)
    ov.lift()
    settle(300)
    report("(b) topmost+lift", before, fg(), ov_hwnd)

    # (c) focus_set
    before = game_front()
    ov.focus_set()
    settle(300)
    report("(c) focus_set", before, fg(), ov_hwnd)

    # (d) real click on the ttk.Scale
    before = game_front()
    ov.update_idletasks()
    x = scale.winfo_rootx() + scale.winfo_width() // 2
    y = scale.winfo_rooty() + scale.winfo_height() // 2
    clicked[0] = 0
    click(x, y)
    settle(350)
    report("(d) click ttk.Scale", before, fg(), ov_hwnd,
           "widget_cmd_fired=%d" % clicked[0])

    # (e) raw Win32 show from withdrawn state
    ov.withdraw()
    settle(300)
    before = game_front()
    _u32.ShowWindow(ov_hwnd, SW_SHOWNOACTIVATE)
    _u32.SetWindowPos(ov_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOW_NOACTIVATE)
    settle(300)
    viewable = ov.winfo_viewable()
    visible = bool(_u32.IsWindowVisible(ov_hwnd))
    # does Tk still pump after() + do widget clicks work?
    fired = [0]
    ov.after(60, lambda: fired.__setitem__(0, 1))
    clicked[0] = 0
    settle(120)
    ov.update_idletasks()
    x = scale.winfo_rootx() + scale.winfo_width() // 2
    y = scale.winfo_rooty() + scale.winfo_height() // 2
    click(x, y)
    settle(300)
    report("(e) ShowWindow+SWP show", before, fg(), ov_hwnd,
           "viewable=%d visible=%d state=%s after_fired=%d click_fired=%d" % (
               viewable, visible, ov.wm_state(), fired[0], clicked[0]))

    ov.withdraw()
    settle(200)


def notepad_hwnd(pid):
    hwnds = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
                                     wintypes.LPARAM)

    @WNDENUMPROC
    def _cb(h, _l):
        ppid = wintypes.DWORD()
        _u32.GetWindowThreadProcessId(h, ctypes.byref(ppid))
        if ppid.value == pid and _u32.IsWindowVisible(h):
            hwnds.append(int(h))
        return True

    _u32.EnumWindows(_cb, 0)
    return hwnds[0] if hwnds else 0


def main():
    root = tk.Tk()
    root.withdraw()

    # Stand-in "game": notepad if it can take the foreground on this host,
    # else a plain Tk toplevel forced to front with the AttachThreadInput dance.
    game_hwnd = 0
    game_name = ""
    np_proc = None
    try:
        np_proc = subprocess.Popen("notepad.exe")
        deadline = time.time() + 4.0
        while time.time() < deadline and not game_hwnd:
            time.sleep(0.1)
            game_hwnd = notepad_hwnd(np_proc.pid)
        if game_hwnd:
            _force_foreground(game_hwnd)
            time.sleep(0.4)
            if fg() == game_hwnd:
                game_name = "notepad"
            else:
                game_hwnd = 0
    except Exception as e:
        print("notepad probe failed: %r" % e, flush=True)

    tk_game = None
    if not game_hwnd:
        tk_game = tk.Toplevel(root)
        tk_game.title("probe-game")
        tk_game.geometry("320x200+40+40")
        tk_game.deiconify()
        root.update()
        _force_foreground(int(tk_game.wm_frame(), 16))
        time.sleep(0.3)
        game_hwnd = fg()
        game_name = "tk-toplevel"

    print("game window: %s %s  (probe pid=%d)" % (game_name, fh(game_hwnd),
                                                 os.getpid()), flush=True)

    # The overlay toplevel, built exactly like ControlOverlay's win.
    ov = tk.Toplevel(root)
    ov.overrideredirect(True)
    ov.attributes("-topmost", True)
    ov.attributes("-alpha", 0.95)
    ov.geometry("300x160+420+260")
    clicked = [0]
    scale = ttk.Scale(ov, from_=0.0, to=1.0,
                      command=lambda _v: clicked.__setitem__(0, 1))
    scale.pack(fill="x", padx=24, pady=50)
    ov.deiconify()
    root.update()
    ov_hwnd = int(ov.wm_frame(), 16)
    _set_noactivate(ov_hwnd, True)
    ov.withdraw()
    root.update()
    time.sleep(0.3)
    print("overlay hwnd: %s" % fh(ov_hwnd), flush=True)

    run_pass(game_hwnd, game_name, root, ov, ov_hwnd, scale, clicked)

    # Cross-check pass: if the game was notepad, also run the cheap steps
    # against our own Tk game window (same-process activation is the strictest
    # case - Windows never blocks it).
    if tk_game is None:
        tk_game = tk.Toplevel(root)
        tk_game.title("probe-game2")
        tk_game.geometry("320x200+40+40")
        tk_game.deiconify()
        root.update()
        time.sleep(0.2)
        run_pass(int(tk_game.wm_frame(), 16), "tk-toplevel(xcheck)",
                 root, ov, ov_hwnd, scale, clicked)

    print("\n==== summary ====", flush=True)
    for r in rows:
        print("%-28s %s -> %s  %s %s" % r, flush=True)

    try:
        ov.destroy()
        if tk_game is not None:
            tk_game.destroy()
        root.destroy()
    except tk.TclError:
        pass
    if np_proc is not None:
        try:
            np_proc.kill()
        except Exception:
            pass
    print("PROBE DONE", flush=True)


if __name__ == "__main__":
    main()
