# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_win32 - Win32 plumbing for the in-game overlay.
# Window style flags, no-activate, cursor freeing and the robust foreground
# steal used only when cfg overlay_focus is on. No tkinter imports here.
# ============================================================================
import ctypes

_u32 = ctypes.windll.user32
SW_RESTORE = 9
SW_SHOWNOACTIVATE = 4
IDC_ARROW = 32512

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080

# SetWindowPos / show flags for a guaranteed non-activating map+restack.
GA_ROOT = 2
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040


def _toplevel_hwnd(win):
    """Resolve a Tk toplevel to its TOP-LEVEL Win32 HWND. wm_frame() returns
    the outer wrapper (hex string); winfo_id() can be an inner child on some
    Tk builds, so the result is climbed to GA_ROOT. Returns None when the
    window has no HWND yet (never forced into existence)."""
    hwnd = None
    try:
        hwnd = int(win.wm_frame(), 16)
    except Exception:                       # TclError/ValueError/TypeError
        hwnd = None
    if not hwnd:
        try:
            hwnd = int(win.winfo_id())
        except Exception:
            return None
    if not hwnd:
        return None
    try:
        root = _u32.GetAncestor(hwnd, GA_ROOT)
        if root:
            hwnd = root
    except (ValueError, OSError):
        pass
    return hwnd


def _set_noactivate(hwnd, on=True):
    """WS_EX_TOPMOST|TOOLWINDOW always; WS_EX_NOACTIVATE only when on.
    Applied to the top-level HWND; MUST run while the window is still
    unmapped (withdrawn) so the very first ShowWindow cannot activate it."""
    try:
        style = _u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        style |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW
        if on:
            style |= WS_EX_NOACTIVATE
        else:
            style &= ~WS_EX_NOACTIVATE
        _u32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)
    except (ValueError, OSError):
        pass


def _exstyle(hwnd):
    """Current GWL_EXSTYLE of hwnd (for the [overlay] shown log line)."""
    try:
        return _u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    except (ValueError, OSError):
        return 0


def _show_noactivate(hwnd):
    """Show + topmost-restack a hwnd WITHOUT any activation path: Tk's
    deiconify honors WS_EX_NOACTIVATE on this host (probe: fg unchanged),
    this is the explicit win32 equivalent so a different Tk build can never
    fall back to SW_SHOWNORMAL. ShowWindow(SW_SHOWNOACTIVATE) maps the window
    in place; SetWindowPos re-asserts HWND_TOPMOST with SWP_NOACTIVATE so the
    z-order fix can never become an activation."""
    try:
        _u32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
        _u32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                          | SWP_SHOWWINDOW)
    except (ValueError, OSError):
        pass


def _free_cursor():
    """Force the OS cursor out of the game's grip: release the clip
    rectangle, release capture, raise the show counter, set a normal arrow."""
    try:
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        for _ in range(10):
            if _u32.ShowCursor(True) >= 0:
                break
        arrow = _u32.LoadCursorW(None, IDC_ARROW)
        if arrow:
            _u32.SetCursor(arrow)

        # Also release clipping, capture, and unhide cursor on the foreground game thread
        fg = _u32.GetForegroundWindow()
        if fg:
            fg_tid = _u32.GetWindowThreadProcessId(fg, None)
            cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
            if fg_tid and fg_tid != cur_tid:
                if _u32.AttachThreadInput(cur_tid, fg_tid, True):
                    try:
                        _u32.ReleaseCapture()
                        _u32.ClipCursor(None)
                        for _ in range(10):
                            if _u32.ShowCursor(True) >= 0:
                                break
                        if arrow:
                            _u32.SetCursor(arrow)
                    finally:
                        _u32.AttachThreadInput(cur_tid, fg_tid, False)
    except (ValueError, OSError):
        pass


class _INPUT(ctypes.Structure):
    """SendInput MOUSEINPUT shim (x64 layout)."""
    _fields_ = [("type", ctypes.c_ulong), ("dx", ctypes.c_long),
                ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


def _input_jiggle():
    """A zero-move SendInput: having 'just received input' is what lets a
    background process win SetForegroundWindow (the foreground lock)."""
    try:
        inp = _INPUT(0, 0, 0, 0, 0x0001, 0, None)   # MOUSEEVENTF_MOVE by 0,0
        _u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(inp))
    except (ValueError, OSError):
        pass


def _force_foreground(hwnd):
    """Robust foreground steal: AttachThreadInput dance around
    SetForegroundWindow/BringWindowToTop after an input jiggle."""
    _input_jiggle()
    try:
        fg = _u32.GetForegroundWindow()
        cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
        fg_tid = _u32.GetWindowThreadProcessId(fg, None) if fg else 0
        attached = False
        if fg_tid and fg_tid != cur_tid:
            attached = bool(_u32.AttachThreadInput(cur_tid, fg_tid, True))
        try:
            _u32.BringWindowToTop(hwnd)
            _u32.SetForegroundWindow(hwnd)
            _u32.SetActiveWindow(hwnd)
            _u32.SetFocus(hwnd)
        finally:
            if attached:
                _u32.AttachThreadInput(cur_tid, fg_tid, False)
    except (ValueError, OSError):
        pass
