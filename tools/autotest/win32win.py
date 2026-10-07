#!/usr/bin/env python3
# ============================================================================
# win32win.py - Win32 window discovery/activation helpers for the capture
# harness. Pure queries + small actions, no session state - capture_session
# owns policy (what to defend, when to minimize).
#
# Window model that was hard-won on GTA IV, kept generic here:
#   - the visible window may belong to a CHILD pid (launcher chains) -> pid-
#     agnostic lookup exists alongside the pid-locked one
#   - the window can be destroyed/recreated any time -> always re-enumerate,
#     never hold a stale hwnd
#   - the window fights SetWindowPos and strips TOPMOST -> we keep it raised
#     in the normal z-band and MINIMIZE occluders instead of a z-order war
# ============================================================================
import ctypes
from ctypes import wintypes
import os
import subprocess

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

GA_ROOT = 2
SW_MINIMIZE = 6
SW_RESTORE = 9
HWND_TOP = 0
SWP_SHOWWINDOW = 0x0040
SWP_NOMOVE_NOSIZE = 0x0043

_QUERY_LIMITED_INFORMATION = 0x1000


def proc_image_base(pid):
    """Lowercase exe basename of a pid ('' when the process is unreadable)."""
    h = k32.OpenProcess(_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(512)
    sz = wintypes.DWORD(512)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(sz))
    k32.CloseHandle(h)
    return (buf.value.rsplit("\\", 1)[-1] if ok and buf.value else "").lower()


def find_game_window(image_base="gtaiv.exe", title_sub="gtaiv", pid=None,
                     want_rect=False):
    """First visible top-level window whose owner process image ends with
    image_base and whose title contains title_sub. pid additionally pins the
    owner pid. Returns hwnd, or (hwnd, (x,y,w,h)) when want_rect."""
    res = []
    WNDENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    image_base = image_base.lower()
    title_sub = title_sub.lower()

    def cb(hwnd, lp):
        if not u32.IsWindowVisible(hwnd):
            return True
        wpid = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if pid is not None:
            if wpid.value != pid:
                return True
        else:
            if proc_image_base(wpid.value) != image_base:
                return True
        n = u32.GetWindowTextLengthW(hwnd)
        tb = ctypes.create_unicode_buffer(n + 8)
        u32.GetWindowTextW(hwnd, tb, n + 8)
        if title_sub in (tb.value or "").lower():
            if want_rect:
                rc = wintypes.RECT()
                u32.GetWindowRect(hwnd, ctypes.byref(rc))
                res.append((hwnd, (rc.left, rc.top,
                                   rc.right - rc.left, rc.bottom - rc.top)))
            else:
                res.append(hwnd)
            return False
        return True

    u32.EnumWindows(WNDENUM(cb), 0)
    return res[0] if res else None


def game_window_rect():
    """(x,y,w,h) of the GTA IV window or None."""
    hit = find_game_window(want_rect=True)
    return hit[1] if hit else None


def game_hwnd(pid):
    """Live hwnd of the GTA IV window owned by a specific pid."""
    return find_game_window(pid=pid)


def game_hwnd_any():
    """Live hwnd of ANY visible GTA IV window - pid-agnostic, because the
    presenting window can belong to a child of the spawned launcher."""
    return find_game_window()


def activate_foreground(hwnd):
    """SetForegroundWindow via the AttachThreadInput path - honoured even
    though this process lacks foreground rights (GTA IV's intro timers only
    tick while its window is FOREGROUND; a z-raise is not enough).
    Returns True when the foreground was actually won."""
    if u32.GetForegroundWindow() == hwnd:
        return True
    fg = u32.GetForegroundWindow()
    our_tid = k32.GetCurrentThreadId()
    fg_tid = u32.GetWindowThreadProcessId(fg, None) if fg else 0
    tgt_tid = u32.GetWindowThreadProcessId(hwnd, None)
    u32.AttachThreadInput(our_tid, fg_tid, 1)
    u32.AttachThreadInput(our_tid, tgt_tid, 1)
    u32.SetForegroundWindow(hwnd)
    u32.BringWindowToTop(hwnd)
    u32.AttachThreadInput(our_tid, fg_tid, 0)
    u32.AttachThreadInput(our_tid, tgt_tid, 0)
    return u32.GetForegroundWindow() == hwnd


# ---- occlusion defense ----------------------------------------------------
# DDA records the DESKTOP surface at a fixed rect, so the game must be the
# top window at every sampled point of that rect. Windows in this set are
# never minimized (system shells, our own pipeline procs, the game itself).
OCCLUDER_EXCLUDE = {
    "textinputhost.exe", "explorer.exe", "searchhost.exe",
    "shellexperiencehost.exe", "startmenuexperiencehost.exe",
    "ddarec.exe", "m11d.exe", "ffmpeg.exe", "gtaiv.exe",
}


def root_window_at(px, py):
    """Top-level ancestor of the topmost window at a desktop point."""
    pt = wintypes.POINT(px, py)
    h = u32.WindowFromPoint(pt)
    if not h:
        return None
    return u32.GetAncestor(h, GA_ROOT)


def occluder_ok(hwnd):
    """A visible top-level window worth minimizing for the run: not ours,
    not the game, not a system overlay that must not be hidden."""
    if not hwnd or not u32.IsWindowVisible(hwnd):
        return False
    pid = wintypes.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value == os.getpid():
        return False
    return proc_image_base(pid.value) not in OCCLUDER_EXCLUDE


def window_rect(hwnd):
    rc = wintypes.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    return (rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top)


def minimize(hwnd):
    u32.ShowWindow(hwnd, SW_MINIMIZE)


def restore(hwnd):
    u32.ShowWindow(hwnd, SW_RESTORE)


def raise_normal_band(hwnd):
    """Raise to the top of the NORMAL z-band (not TOPMOST - GTA IV strips
    TOPMOST instantly but never re-arms a plain raise)."""
    u32.SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     0x0001 | 0x0002 | SWP_SHOWWINDOW)


def set_pos_top(hwnd, x, y, w, h):
    u32.SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW)


def exe_running(image_name):
    """True if image_name (e.g. 'GTAIV.exe') shows in tasklist."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % image_name, "/NH"],
            capture_output=True, text=True, timeout=15).stdout
        return image_name.lower() in out.lower()
    except Exception:
        return False
