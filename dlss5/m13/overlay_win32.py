# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_win32 - Win32 plumbing for the in-game overlay.
# Window style flags, no-activate, cursor freeing and the robust foreground
# steal used only when cfg overlay_focus is on. No tkinter imports here
# (fix_popdown only talks to the widget's own .tk interpreter object).
# ============================================================================
import ctypes
from ctypes import wintypes

_u32 = ctypes.windll.user32
_k32 = ctypes.windll.kernel32
SW_RESTORE = 9
SW_SHOWNOACTIVATE = 4
IDC_ARROW = 32512

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TRANSPARENT = 0x00000020

# SetWindowPos / show flags for a guaranteed non-activating map+restack.
GA_ROOT = 2
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020
SWP_SHOWWINDOW = 0x0040

MONITOR_DEFAULTTONEAREST = 2


class _INPUT(ctypes.Structure):
    """SendInput MOUSEINPUT shim (x64 layout)."""
    _fields_ = [("type", ctypes.c_ulong), ("dx", ctypes.c_long),
                ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class _MONITORINFO(ctypes.Structure):
    """MONITORINFO (Win32): rcMonitor = full bounds, rcWork = minus taskbar."""
    _fields_ = [("cbSize", wintypes.DWORD),
                ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD)]


# --------------------------------------------------------------------------
# ctypes signatures. ctypes defaults every arg to c_int, which truncates
# 64-bit HWNDs/pointers on x64 - and _u32 is the SAME shared WinDLL object
# other modules (processes.py, icons.py, probes) call through, so these are
# declared once here, carefully, so every caller keeps working:
#   - HWND args accept plain ints, None and c_void_p instances;
#   - pointer args accept ctypes.byref(...) objects;
#   - EnumWindows takes c_void_p so ANY WNDENUMPROC closure still fits.
# --------------------------------------------------------------------------
_u32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
_u32.GetWindowLongPtrW.restype = ctypes.c_ssize_t        # LONG_PTR
_u32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int,
                                   ctypes.c_ssize_t]
_u32.SetWindowLongPtrW.restype = ctypes.c_ssize_t        # LONG_PTR
_u32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
_u32.GetWindowLongW.restype = wintypes.LONG
_u32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
_u32.GetAncestor.restype = wintypes.HWND
_u32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_u32.GetWindow.restype = wintypes.HWND
_u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              wintypes.UINT]
_u32.SetWindowPos.restype = wintypes.BOOL
_u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_u32.ShowWindow.restype = wintypes.BOOL
_u32.IsWindowVisible.argtypes = [wintypes.HWND]
_u32.IsWindowVisible.restype = wintypes.BOOL
_u32.GetWindowRect.argtypes = [wintypes.HWND, wintypes.LPRECT]
_u32.GetWindowRect.restype = wintypes.BOOL
_u32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
_u32.EnumWindows.restype = wintypes.BOOL
_u32.GetForegroundWindow.restype = wintypes.HWND
_u32.SetForegroundWindow.argtypes = [wintypes.HWND]
_u32.SetForegroundWindow.restype = wintypes.BOOL
_u32.BringWindowToTop.argtypes = [wintypes.HWND]
_u32.BringWindowToTop.restype = wintypes.BOOL
_u32.SetActiveWindow.argtypes = [wintypes.HWND]
_u32.SetActiveWindow.restype = wintypes.HWND
_u32.SetFocus.argtypes = [wintypes.HWND]
_u32.SetFocus.restype = wintypes.HWND
_u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
                                          ctypes.POINTER(wintypes.DWORD)]
_u32.GetWindowThreadProcessId.restype = wintypes.DWORD
_u32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD,
                                   wintypes.BOOL]
_u32.AttachThreadInput.restype = wintypes.BOOL
_u32.ReleaseCapture.argtypes = []
_u32.ReleaseCapture.restype = wintypes.BOOL
_u32.ClipCursor.argtypes = [wintypes.LPRECT]
_u32.ClipCursor.restype = wintypes.BOOL
_u32.ShowCursor.argtypes = [wintypes.BOOL]
_u32.ShowCursor.restype = ctypes.c_int
_u32.SetCursor.argtypes = [ctypes.c_void_p]
_u32.SetCursor.restype = ctypes.c_void_p                  # HCURSOR
_u32.LoadCursorW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]
_u32.LoadCursorW.restype = ctypes.c_void_p                # HCURSOR
_u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
_u32.SetCursorPos.restype = wintypes.BOOL
_u32.GetCursorPos.argtypes = [wintypes.LPPOINT]
_u32.GetCursorPos.restype = wintypes.BOOL
_u32.GetAsyncKeyState.argtypes = [ctypes.c_int]
_u32.GetAsyncKeyState.restype = wintypes.SHORT
_u32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
_u32.MonitorFromWindow.restype = wintypes.HMONITOR
# arg2 is c_void_p, not POINTER(_MONITORINFO): this module can be imported
# twice (m13.* and dlss5.m13.*) producing two _MONITORINFO classes; a typed
# pointer argtypes then rejects the other module's byref on the shared _u32.
_u32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.c_void_p]
_u32.GetMonitorInfoW.restype = wintypes.BOOL
_u32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(_INPUT),
                           ctypes.c_int]
_u32.SendInput.restype = wintypes.UINT
_k32.GetCurrentThreadId.argtypes = []
_k32.GetCurrentThreadId.restype = wintypes.DWORD
_u32.SystemParametersInfoW.argtypes = [wintypes.UINT, wintypes.UINT,
                                       ctypes.c_void_p, wintypes.UINT]
_u32.SystemParametersInfoW.restype = wintypes.BOOL
_u32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND,
                                 wintypes.UINT, wintypes.WPARAM,
                                 wintypes.LPARAM]
_u32.CallWindowProcW.restype = ctypes.c_ssize_t           # LRESULT


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
    unmapped (withdrawn) so the very first ShowWindow cannot activate it.
    The trailing SWP_FRAMECHANGED commit makes Windows re-read the style
    immediately when the window IS already mapped (popdowns, tooltips)."""
    try:
        style = _u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        style |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW
        if on:
            style |= WS_EX_NOACTIVATE
        else:
            style &= ~WS_EX_NOACTIVATE
        _u32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)
        _u32.SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                          | SWP_NOACTIVATE | SWP_FRAMECHANGED)
    except (ValueError, OSError):
        pass


def set_noactivate_for_hwnd(hwnd):
    """Public wrapper over _set_noactivate for callers that already hold a
    Win32 HWND (ToolTip toplevels, combobox popdowns). Always applies the
    full no-activate tool-window exstyle; silently no-ops on bad input."""
    try:
        _set_noactivate(int(hwnd), True)
    except (TypeError, ValueError):
        pass


def set_clickthrough(hwnd, on=True):
    """Add/remove WS_EX_TRANSPARENT on the top-level HWND.

    The in-layer bridge keeps the Tk window on-screen but invisible
    (-alpha 0) so Tk keeps painting a real surface for PrintWindow.
    WS_EX_TRANSPARENT makes pointer hit-testing fall through to the game
    below, where the layer captures real input and turns it into ring
    events. A fully-transparent layered window already skips hit-testing;
    this is belt on top of it."""
    try:
        style = _u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        if on:
            style |= WS_EX_TRANSPARENT
        else:
            style &= ~WS_EX_TRANSPARENT
        _u32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)
        _u32.SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                          | SWP_NOACTIVATE | SWP_FRAMECHANGED)
    except (ValueError, OSError):
        pass


def fix_popdown(combo):
    """Return a <Button-1> bind handler (call combo.bind with the result)
    that hardens a ttk.Combobox popdown: the popdown is a fresh Tk toplevel
    that does NOT inherit the panel's WS_EX_NOACTIVATE - showing it plain
    would be an activation vector against exclusive-fullscreen games. Keeps
    it -topmost and applies the noactivate exstyle at three points because
    Tk re-writes GWL_EXSTYLE while mapping the popdown: now (HWND already
    exists on reopen), on the popdown's own <Map> (our binding lands after
    Tk's restyle), and once more ~60 ms later as a belt."""
    _mapped_popdowns = set()   # popdown paths already <Map>-bound

    def _apply(_e=None):
        try:
            popdown = combo.tk.eval(
                "ttk::combobox::PopdownWindow %s" % combo)
            combo.tk.eval("wm attributes %s -topmost 1" % popdown)
        except Exception:
            return

        def _noactivate_popdown(_e2=None):
            try:
                ph = combo.tk.eval("wm frame %s" % popdown)
                _set_noactivate(int(ph, 16), True)
            except Exception:
                pass
        _noactivate_popdown()                  # now, if the HWND exists
        try:
            # <Map> runs after Tk's own map-time restyle; add="+" must not
            # clobber the popdown's internal bindings, so dedupe instead.
            if popdown not in _mapped_popdowns:
                _mapped_popdowns.add(popdown)
                combo.nametowidget(popdown).bind(
                    "<Map>", _noactivate_popdown, add="+")
        except Exception:
            pass
        combo.after_idle(_noactivate_popdown)  # right after the map
        combo.after(60, _noactivate_popdown)   # after Tk's restyle settles
    return _apply


# ------------------------------------------------------- mouse-activate ---
# WM_MOUSEACTIVATE -> MA_NOACTIVATE subclass for the framed external
# overlay: WS_EX_NOACTIVATE already keeps the toplevel out of the
# foreground, but the field report showed a click still minimizing the
# exclusive-fullscreen game on this host (the default wndproc answer is
# MA_ACTIVATE). The subclass answers "no activate" itself while the click
# still reaches Tk for widget hit-testing - Tk buttons/scales respond to
# clicks on a never-active window (only text Entry keyboard input needs
# real focus; overlay_focus handles that separately).
GWLP_WNDPROC = -4
WM_MOUSEACTIVATE = 0x0021
MA_NOACTIVATE = 3
SPI_GETWORKAREA = 0x0030
_WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND,
                              wintypes.UINT, wintypes.WPARAM,
                              wintypes.LPARAM)
_subclassed = {}            # hwnd -> (callback, prev_proc) keepalive


def subclass_noactivate(hwnd):
    """Install the WM_MOUSEACTIVATE->MA_NOACTIVATE wndproc filter on a
    toplevel HWND. Idempotent per HWND; the callback stays referenced in
    _subclassed so ctypes never frees a live proc."""
    try:
        hwnd = int(hwnd)
    except (TypeError, ValueError):
        return
    if not hwnd or hwnd in _subclassed:
        return
    try:
        prev = _u32.GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
        if not prev:
            return

        def _proc(h, m, w, l):
            if m == WM_MOUSEACTIVATE:
                return MA_NOACTIVATE
            try:
                return _u32.CallWindowProcW(prev, h, m, w, l)
            except Exception:
                return 0
        cb = _WNDPROC(_proc)
        old = _u32.SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC,
            ctypes.cast(cb, ctypes.c_void_p).value or 0)
        if old:
            _subclassed[hwnd] = (cb, prev)
    except (ValueError, OSError):
        pass


def primary_work_area():
    """(l, t, r, b) of the primary monitor WORK area (SPI_GETWORKAREA) -
    the framed overlay's fallback rect when no game hwnd is known."""
    rc = wintypes.RECT()
    try:
        if _u32.SystemParametersInfoW(SPI_GETWORKAREA, 0,
                                      ctypes.byref(rc), 0):
            return (rc.left, rc.top, rc.right, rc.bottom)
    except (ValueError, OSError):
        pass
    return None


def monitor_work_area_for_hwnd(hwnd):
    """(l, t, r, b) of the WORK area (minus taskbar) of the monitor the
    window lives on, via MonitorFromWindow(DEFAULTTONEAREST) +
    GetMonitorInfoW. Multi-monitor safe; None when the probe fails."""
    try:
        mon = _u32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        if not mon:
            return None
        info = _MONITORINFO()
        info.cbSize = ctypes.sizeof(_MONITORINFO)
        if not _u32.GetMonitorInfoW(mon, ctypes.byref(info)):
            return None
        return (info.rcWork.left, info.rcWork.top,
                info.rcWork.right, info.rcWork.bottom)
    except (ValueError, OSError):
        return None


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


def _free_cursor(target_hwnd=None):
    """Force the OS cursor out of the game's grip: release the clip
    rectangle, release capture, raise the show counter, set a normal arrow.
    target_hwnd: the game window whose thread state needs freeing -
    defaults to the foreground window when the caller does not know it."""
    try:
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        for _ in range(10):
            if _u32.ShowCursor(True) >= 0:
                break
        arrow = _u32.LoadCursorW(None, IDC_ARROW)
        if arrow:
            _u32.SetCursor(arrow)

        # Also release clipping, capture, and unhide cursor on the game's
        # thread (foreground is only a guess when target_hwnd is unknown).
        fg = target_hwnd or _u32.GetForegroundWindow()
        if fg:
            fg_tid = _u32.GetWindowThreadProcessId(fg, None)
            cur_tid = _k32.GetCurrentThreadId()
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
        cur_tid = _k32.GetCurrentThreadId()
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


# --------------------------------------------------------------- printing --
# In-layer overlay pixel source: PrintWindow(hwnd, memdc,
# PW_RENDERFULLCONTENT) renders the panel HWND into a 32-bpp top-down DIB.
# The window stays MAPPED and ON-SCREEN but invisible (-alpha 0 +
# WS_EX_TRANSPARENT - see overlay.py): a fully off-screen window has an
# empty GDI visible region, so Tk's paint gets clipped away and the DIB
# comes back all-zero (probed on this host 2026-10-06).
# Same ctypes style as icons.py; the _g32/_u32 signatures below also fix
# the default-c_int truncation for icons.py's own calls (both modules
# share the same WinDLL objects).
_g32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 0x00000002
BI_RGB = 0
DIB_RGB_COLORS = 0

_u32.PrintWindow.argtypes = [wintypes.HWND, ctypes.c_void_p, wintypes.UINT]
_u32.PrintWindow.restype = wintypes.BOOL
_u32.GetDC.argtypes = [wintypes.HWND]
_u32.GetDC.restype = ctypes.c_void_p                      # HDC
_u32.ReleaseDC.argtypes = [wintypes.HWND, ctypes.c_void_p]
_u32.ReleaseDC.restype = ctypes.c_int
_u32.GetClientRect.argtypes = [wintypes.HWND, wintypes.LPRECT]
_u32.GetClientRect.restype = wintypes.BOOL
_u32.ClientToScreen.argtypes = [wintypes.HWND,
                                ctypes.POINTER(wintypes.POINT)]
_u32.ClientToScreen.restype = wintypes.BOOL
_g32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
_g32.CreateCompatibleDC.restype = ctypes.c_void_p         # HDC
_g32.CreateDIBSection.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                  wintypes.UINT, ctypes.c_void_p,
                                  wintypes.HANDLE, wintypes.DWORD]
_g32.CreateDIBSection.restype = ctypes.c_void_p           # HBITMAP
_g32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_g32.SelectObject.restype = ctypes.c_void_p               # HGDIOBJ
_g32.DeleteObject.argtypes = [ctypes.c_void_p]
_g32.DeleteObject.restype = wintypes.BOOL
_g32.DeleteDC.argtypes = [ctypes.c_void_p]
_g32.DeleteDC.restype = wintypes.BOOL


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD),
                ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long),
                ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER),
                ("bmiColors", wintypes.DWORD)]


def window_size(hwnd):
    """(w, h) of a window's outer rect. Pure win32 - callable off the Tk
    thread (the publish worker uses it instead of winfo_*)."""
    rect = wintypes.RECT()
    if _u32.GetWindowRect(hwnd, ctypes.byref(rect)):
        return (rect.right - rect.left, rect.bottom - rect.top)
    return None


def client_size(hwnd):
    """(w, h) of a window's CLIENT area - the pixel surface the layer
    composites into (frame coords == client coords)."""
    rect = wintypes.RECT()
    if _u32.GetClientRect(hwnd, ctypes.byref(rect)):
        return (rect.right - rect.left, rect.bottom - rect.top)
    return None


def client_origin(hwnd):
    """Screen (x, y) of a window's client-area origin. Subtracting it from
    a screen-space panel position yields the game's CLIENT coords."""
    pt = wintypes.POINT(0, 0)
    if _u32.ClientToScreen(hwnd, ctypes.byref(pt)):
        return (pt.x, pt.y)
    return None


def client_rect_screen(hwnd):
    """(x, y, w, h) of a window's client area in SCREEN coords - the rect
    the framed overlay hugs (windowed games: the real play area; exclusive
    fullscreen: the whole mode). None when either query fails."""
    cs = client_size(hwnd)
    org = client_origin(hwnd)
    if cs and org and cs[0] > 0 and cs[1] > 0:
        return (org[0], org[1], cs[0], cs[1])
    return None


class DibCapture:
    """Reusable PrintWindow -> BGRA pipeline for one window size at a
    time. Owns a memory DC + 32bpp top-down DIBSection; grab() returns
    straight bytes with the alpha lane forced to `alpha` (GDI renders
    leave A=0 in a 32bpp DIB - the layer composites straight-alpha and the
    panel is a solid rect, so a uniform 0xE0 reproduces the external
    window's -alpha 0.88 look)."""

    def __init__(self):
        self._hdc = self._hbmp = self._old = self._bits = None
        self._wh = (0, 0)

    def _teardown(self):
        if self._hdc and self._old:
            _g32.SelectObject(self._hdc, self._old)
        if self._hbmp:
            _g32.DeleteObject(self._hbmp)
        if self._hdc:
            _g32.DeleteDC(self._hdc)
        self._hdc = self._hbmp = self._old = self._bits = None
        self._wh = (0, 0)

    def _ensure(self, w, h):
        """(Re)create the DC+DIB when the size changed; True when ready."""
        if (w, h) == self._wh and self._hdc:
            return True
        self._teardown()
        screen = _u32.GetDC(None)
        if not screen:
            return False
        try:
            self._hdc = _g32.CreateCompatibleDC(screen)
            if not self._hdc:
                return False
            bmi = _BITMAPINFO()
            hdr = bmi.bmiHeader
            hdr.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
            hdr.biWidth = w
            hdr.biHeight = -h           # negative = top-down rows
            hdr.biPlanes = 1
            hdr.biBitCount = 32
            hdr.biCompression = BI_RGB
            bits = ctypes.c_void_p()
            self._hbmp = _g32.CreateDIBSection(
                self._hdc, ctypes.byref(bmi), DIB_RGB_COLORS,
                ctypes.byref(bits), None, 0)
            if not self._hbmp or not bits.value:
                self._teardown()
                return False
            self._bits = bits.value
            self._old = _g32.SelectObject(self._hdc, self._hbmp)
            self._wh = (w, h)
            return True
        finally:
            _u32.ReleaseDC(None, screen)

    def grab(self, hwnd, w, h, alpha=0xE0, hole=None):
        """-> BGRA bytes (w*h*4) of the window's current content, or None.
        The whole rect gets `alpha` - the panel band is a solid fill.

        hole: optional (l, t, r, b) rect in pixel coords whose pixels are
        zeroed (alpha 0) instead - the framed layout's see-through center.
        Needed even though the layer skips the hole while blending: it
        keeps the published payload self-consistent (a hole-unaware
        reader still shows a clean center). Row-contiguous zero fills -
        a few ms at 2560x1440 on the 15 Hz publish pump."""
        if w <= 0 or h <= 0 or not self._ensure(w, h):
            return None
        if not _u32.PrintWindow(hwnd, self._hdc, PW_RENDERFULLCONTENT):
            return None
        n = w * h
        buf = bytearray(ctypes.string_at(self._bits, n * 4))
        buf[3::4] = bytes((alpha,)) * n
        if hole:
            l, t, r, b = (int(v) for v in hole)
            l = min(max(0, l), w)
            r = min(max(0, r), w)
            t = min(max(0, t), h)
            b = min(max(0, b), h)
            if l < r and t < b:
                row = bytes((r - l) * 4)
                stride = w * 4
                for y in range(t, b):
                    o = y * stride + l * 4
                    buf[o:o + len(row)] = row
        return bytes(buf)

    def close(self):
        self._teardown()
