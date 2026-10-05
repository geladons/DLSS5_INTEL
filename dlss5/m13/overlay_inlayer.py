# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_inlayer - InLayerUI: bridge between the invisible Tk panel
# and the in-game Vulkan layer (cfg overlay_mode "inlayer", the default).
#
# The overlay toplevel stays MAPPED, on-screen but invisible (-alpha 0 +
# WS_EX_TRANSPARENT click-through); a publish pump PrintWindow's it into
# "Local\DLSS5_UI_<pid>" (~15 Hz + heartbeat) and the in-process Vulkan
# layer alpha-blends it into the frame. While ui_visible is set the
# layer's wndproc filter EATS the game's window input and posts it to the
# shared event ring; a ~10 ms Tk-side drain pump turns those events back
# into Tk events on the panel widgets - hit-testing comes for free from
# the widget tree. All foreground/cursor machinery is external-only:
# in-layer never moves the OS cursor and never shows a visible window on
# the desktop. ControlOverlay (overlay.py) owns the lifecycle via the
# `self._il` back-reference - InLayerUI(overlay) reaches back into the
# overlay object for win/ctl/cfg/dst, matching the dispatch contract.
# ============================================================================
import ctypes
import time
import tkinter as tk
from ctypes import wintypes

from .overlay_shm import (ShmUI, MAX_W as SHM_MAX_W, MAX_H as SHM_MAX_H,
                          EVT_MOVE, EVT_BTNDN, EVT_BTNUP, EVT_WHEEL,
                          EVT_KEYDN, EVT_KEYUP, EVT_CHAR)
from .overlay_win32 import (_u32, _toplevel_hwnd,
                            client_origin, client_size, window_size,
                            DibCapture)

# ------------------------------------------------------------ in-layer ----
# The in-layer toplevel cannot be parked off-screen: a window whose GDI
# visible region is empty (fully outside the virtual desktop) never gets
# its paint clipped through to a real surface, so PrintWindow returns an
# all-zero DIB and Tk lacks WM_PRINTCLIENT rendering (probed on this host
# 2026-10-06). Instead it is mapped ON-SCREEN but invisible: -alpha 0 (a
# fully-transparent layered window is already hit-test transparent) plus
# WS_EX_TRANSPARENT so every real click falls through to the game below,
# where the layer's wndproc filter captures it into the ring. The park
# spot is the panel's own composited position (client origin + dst),
# clamped into the virtual desktop in case the game is minimized.

# Win32 VK -> Tk keysym for ring key events (types 5/6). The overlay has
# no text fields - this exists for completeness and future bindings.
_VK_KEYSYM = {
    0x08: "BackSpace", 0x09: "Tab", 0x0D: "Return", 0x10: "Shift_L",
    0x11: "Control_L", 0x12: "Alt_L", 0x13: "Pause", 0x14: "Caps_Lock",
    0x1B: "Escape", 0x20: "space", 0x21: "Prior", 0x22: "Next",
    0x23: "End", 0x24: "Home", 0x25: "Left", 0x26: "Up", 0x27: "Right",
    0x28: "Down", 0x2C: "Print", 0x2D: "Insert", 0x2E: "Delete",
    0x5B: "Super_L", 0x5C: "Super_R", 0x5D: "Menu",
    0xA0: "Shift_L", 0xA1: "Shift_R", 0xA2: "Control_L",
    0xA3: "Control_R", 0xA4: "Alt_L", 0xA5: "Alt_R",
    0x6A: "KP_Multiply", 0x6B: "KP_Add", 0x6D: "KP_Subtract",
    0x6E: "KP_Decimal", 0x6F: "KP_Divide",
    0x90: "Num_Lock", 0x91: "Scroll_Lock",
    0xBA: "semicolon", 0xBB: "equal", 0xBC: "comma", 0xBD: "minus",
    0xBE: "period", 0xBF: "slash", 0xC0: "grave", 0xDB: "bracketleft",
    0xDC: "backslash", 0xDD: "bracketright", 0xDE: "apostrophe",
}
for _i in range(10):                                # 0x30-0x39 digits
    _VK_KEYSYM[0x30 + _i] = str(_i)
for _i in range(26):                                # 0x41-0x5A letters
    _VK_KEYSYM[0x41 + _i] = chr(ord("a") + _i)
for _i in range(10):                                # 0x60-0x69 numpad
    _VK_KEYSYM[0x60 + _i] = "KP_%d" % _i
for _i in range(1, 13):                             # 0x70-0x7B F1-F12
    _VK_KEYSYM[0x6F + _i] = "F%d" % _i

# Layer button index -> Tk button number (nr_ui.c: 0=L 1=R 2=M 3=X1 4=X2).
_IL_BTN = {0: 1, 1: 3, 2: 2, 3: 4, 4: 5}


def _cursor_sprite_rgba():
    """32x32 RGBA straight-alpha arrow for the layer-drawn cursor:
    hardcoded polygon, white fill with a 1 px black outline, transparent
    elsewhere (the classic pointer silhouette)."""
    poly = [(4, 2), (4, 22), (9, 17), (12, 26), (15, 24), (12, 16),
            (19, 16)]

    def inside(px, py):           # even-odd point-in-polygon
        hit = False
        j = len(poly) - 1
        for i in range(len(poly)):
            xi, yi = poly[i]
            xj, yj = poly[j]
            if (yi > py) != (yj > py) and \
                    px < (xj - xi) * (py - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    buf = bytearray(32 * 32 * 4)
    for y in range(32):
        for x in range(32):
            if not inside(x + 0.5, y + 0.5):
                continue
            edge = not (inside(x - 0.5, y + 0.5)
                        and inside(x + 1.5, y + 0.5)
                        and inside(x + 0.5, y - 0.5)
                        and inside(x + 0.5, y + 1.5))
            o = (y * 32 + x) * 4
            buf[o:o + 4] = b"\x00\x00\x00\xff" if edge \
                else b"\xff\xff\xff\xff"
    return bytes(buf)


_CURSOR_RGBA = _cursor_sprite_rgba()


class InLayerUI:
    """Bridge between the invisible Tk panel and the in-game Vulkan layer.

    Two self-rescheduling after() pumps, BOTH on the Tk thread:
      * publish pump (~15 Hz while armed): update_idletasks() forces any
        pending paint, PrintWindow's the invisible toplevel into a BGRA
        DIB, pushes it into the section under the seqlock and bumps the
        heartbeat. PrintWindow MUST run on the window's own thread: it is
        a synchronous WM_PRINT send which blocks until the owning thread
        pumps messages - on a worker thread it freezes whenever the
        mainloop is out of update() (probed 2026-10-06). A few ms of GDI
        + a ~1.4 MB memcpy at 15 Hz is well under the "no blocking I/O
        on the Tk thread" bar;
      * drain pump (~10 ms while VISIBLE): polls the event ring and
        re-creates the events on the panel widgets via event_generate so
        the existing bindings do hit-testing, commands and drags for
        free.

    Pumps carry a generation stamp: a callback scheduled under an older
    arm/disarm cycle self-terminates, so a stale pending after() can
    never resurrect a dead pump.

    Synthetic pointer events carry GAME CLIENT coords in x_root/y_root -
    the parked window's own screen position is meaningless, and keeping
    "root" = client coords lets the drag handlers work unchanged.
    """

    PUBLISH_MS = 66        # ~15 Hz bitmap + heartbeat while armed
    DRAIN_MS = 10          # event ring poll while visible
    PIDCHECK_S = 1.0       # game-pid liveness revalidation cadence

    def __init__(self, overlay):
        self.ov = overlay
        self.shm = ShmUI()
        self.dib = DibCapture()
        self.pid = None
        self.dst = (0, 0)             # panel origin, GAME CLIENT coords
        self._client = (0, 0)         # game client extent (drag clamp)
        self._hwnd = None             # panel HWND (resampled each tick)
        self._size = (0, 0)           # panel pixel extent
        self._pub_id = None
        self._drain_id = None
        self._gen = 0                 # arm-generation stamp for pumps
        self._hover = None            # synthetic pointer position widget
        self._capture = None          # widget holding the implicit grab
        self._held = set()            # held Tk button numbers
        self._ev_client = (0, 0)      # last move, client coords
        self._ev_xy = (0, 0)          # last move, panel-local coords
        self._last_drop = 0
        self._last_pid_check = 0.0
        self._logged_fail = False

    def _log(self, msg):
        try:
            self.ov.on_log(msg)
        except Exception:
            pass

    # --------------------------------------------------------- lifetime ---
    def is_armed(self):
        return self.shm.is_open()

    def arm(self, pid):
        """Tk thread: create-or-open the game session's section and start
        the publish pump. False -> caller falls back to external mode."""
        self.disarm()
        if not self.shm.open(pid):
            return False
        self.pid = int(pid)
        self.shm.write_cursor_sprite(_CURSOR_RGBA)
        self.shm.set_cursor_visible(True)
        self._last_pid_check = 0.0
        self._last_drop = 0
        self._logged_fail = False
        self._gen += 1
        self._schedule_publish()
        return True

    def disarm(self):
        """Tk thread: close the section (the publish pump stops, the
        heartbeat dies -> the layer releases input capture + hides the
        panel). A pending pump callback notices the bumped generation /
        closed section and self-terminates."""
        self._gen += 1
        self._cancel_drain()
        self._cancel_publish()
        self.shm.close()
        self.pid = None
        self._hover = self._capture = None
        self._held.clear()
        self._client = (0, 0)

    def armed(self):
        """Tk thread, at show(): first synchronous frame (so the very
        first composited frame carries real pixels), then the flag and
        the drain pump."""
        win = self.ov.win
        try:
            win.update_idletasks()
            self._size = (win.winfo_width(), win.winfo_height())
        except tk.TclError:
            pass
        self._publish_once()
        self.shm.set_visible(True)
        self._schedule_drain()

    def hidden(self):
        """Tk thread, at hide(): drop ui_visible - the layer stops
        compositing AND capturing; the publish pump keeps running
        (heartbeat stays alive) so the next show re-arms instantly."""
        self._cancel_drain()
        self.shm.set_visible(False)
        self._hover = self._capture = None
        self._held.clear()

    # --------------------------------------------------------- placement --
    def place(self, game_hwnd):
        """Centre over the game window - the same visual convention as
        ControlOverlay._center - expressed in GAME CLIENT coords (the
        layer composites at dst_x/dst_y inside the frame)."""
        W, H = self.ov.WIDTH, self.ov.HEIGHT
        rect = wintypes.RECT()
        if game_hwnd and _u32.GetWindowRect(game_hwnd, ctypes.byref(rect)):
            gw = rect.right - rect.left
            gh = rect.bottom - rect.top
            sx = rect.left + max(0, (gw - W) // 2)
            sy = rect.top + max(0, (gh - H) // 2)
            org = client_origin(game_hwnd)
            cs = client_size(game_hwnd)
            if org and cs:
                self._client = cs
                dx = min(max(0, sx - org[0]), max(0, cs[0] - W))
                dy = min(max(0, sy - org[1]), max(0, cs[1] - H))
                self.dst = (dx, dy)
                return True
        # No window info: assume the game fills the primary screen (true
        # for the fullscreen case this mode is built for).
        try:
            sw = self.ov.win.winfo_screenwidth()
            sh = self.ov.win.winfo_screenheight()
        except tk.TclError:
            sw = sh = 0
        self._client = (sw, sh)
        self.dst = (max(0, (sw - W) // 2), max(0, (sh - H) // 2))
        return False

    def set_dst(self, nx, ny):
        """Drag the composited panel to new client coords, clamped into
        the game client rect so it can never leave the frame."""
        cw, ch = self._client
        if cw and ch:
            nx = min(max(0, nx), max(0, cw - self.ov.WIDTH))
            ny = min(max(0, ny), max(0, ch - self.ov.HEIGHT))
        self.dst = (int(nx), int(ny))

    def last_client_xy(self):
        return self._ev_client

    def park_pos(self):
        """Screen coords for the invisible Tk window: the game client
        origin + the composited dst, so the (transparent) window sits
        exactly where the layer draws the panel and real click-through
        input lines up. Clamped into the virtual desktop - a minimized
        game reports a -32000 origin and an off-screen window never gets
        its Tk paint through (see the module header note)."""
        W, H = self.ov.WIDTH, self.ov.HEIGHT
        gh = self.ov._game_hwnd
        org = client_origin(gh) if gh else None
        if org:
            sx, sy = org[0] + self.dst[0], org[1] + self.dst[1]
        else:
            sx = sy = 0
        l, t, r, b = self.ov._vroot_bounds()
        if sx + W <= l or sx >= r or sy + H <= t or sy >= b:
            sx, sy = l + 8, t + 8       # degenerate origin: primary corner
        return sx, sy

    # ------------------------------------------------------ publish pump --
    def _schedule_publish(self):
        self._cancel_publish()
        if not self.shm.is_open():
            return                   # disarmed mid-tick: pump stays dead
        gen = self._gen
        try:
            self._pub_id = self.ov.win.after(
                self.PUBLISH_MS, lambda g=gen: self._publish_tick(g))
        except Exception:
            self._pub_id = None

    def _cancel_publish(self):
        if self._pub_id is not None:
            try:
                self.ov.win.after_cancel(self._pub_id)
            except Exception:
                pass
            self._pub_id = None

    def _publish_tick(self, gen):
        """Tk thread @~15 Hz while armed: flush paint, PrintWindow ->
        shm, heartbeat, ~1 Hz game-pid liveness. Everything stays on the
        Tk thread on purpose - PrintWindow on the panel HWND is a
        synchronous same-thread WM_PRINT send here, while from a worker
        it blocks until this thread next pumps messages."""
        self._pub_id = None
        if gen != self._gen:
            return                       # stale pump from an old arm
        if not self.shm.is_open():
            # Section died under us (never happens from this class now
            # that the pid check is in-line, but a failed shm op could
            # still close it) - drop the in-layer session.
            if self.ov._inlayer:
                self.ov._inlayer_lost()
            return
        try:
            win = self.ov.win
            win.update_idletasks()       # apply pending paint now
            self._size = (win.winfo_width(), win.winfo_height())
            if not self._hwnd:
                self._hwnd = _toplevel_hwnd(win)
        except tk.TclError:
            pass
        try:
            if not self._publish_once() and not self._logged_fail:
                self._logged_fail = True
                self._log("[overlay] shm publish stalled "
                          "(PrintWindow failed)")
            self.shm.bump_heartbeat()
        except Exception as e:
            if not self._logged_fail:
                self._logged_fail = True
                self._log("[overlay] shm publish error: %s" % e)
        now = time.monotonic()
        if now - self._last_pid_check >= self.PIDCHECK_S:
            self._last_pid_check = now
            self._check_pid()
        self._schedule_publish()

    def _publish_once(self):
        hwnd = self._hwnd
        if not hwnd:
            return False
        w, h = self._size
        if not w or not h:
            size = window_size(hwnd)
            if not size:
                return False
            w, h = size
        if w > SHM_MAX_W or h > SHM_MAX_H:
            return False                 # panel exceeds the ABI cap
        bgra = self.dib.grab(hwnd, w, h)
        if bgra is None:
            return False
        return self.shm.write_bitmap(bgra, w, h, *self.dst)

    def _check_pid(self):
        """~1 Hz liveness (Tk thread): the layer lives in-process, so
        when the armed game pid is gone the consumer is too - drop the
        session; the next show() re-evaluates from scratch."""
        try:
            pid = self.ov.ctl.active_game_pid()
        except Exception:
            return
        if pid == self.pid:
            return
        self._log("[overlay] in-layer game pid %s gone - disarming"
                  % self.pid)
        self.ov._inlayer_lost()

    # --------------------------------------------------------- drain pump --
    def _schedule_drain(self):
        self._cancel_drain()
        gen = self._gen
        try:
            self._drain_id = self.ov.win.after(
                self.DRAIN_MS, lambda g=gen: self._drain(g))
        except Exception:
            self._drain_id = None

    def _cancel_drain(self):
        if self._drain_id is not None:
            try:
                self.ov.win.after_cancel(self._drain_id)
            except Exception:
                pass
            self._drain_id = None

    def _drain(self, gen):
        """Tk thread: turn the layer's ring events into Tk events."""
        self._drain_id = None
        if gen != self._gen or not (
                self.shm.is_open() and self.ov._inlayer
                and self.ov._ui_visible):
            return
        try:
            events = self.shm.poll_events()
        except Exception:
            events = []
        if self.shm.evt_dropped != self._last_drop:
            self._last_drop = self.shm.evt_dropped
            self._log("[overlay] input ring overflow - %d event(s) "
                      "dropped by the layer" % self._last_drop)
        for ev in events:
            try:
                self._inject(*ev)
            except Exception:
                pass             # one bad event must not kill the pump
        self._schedule_drain()

    # --------------------------------------------------- event injection --
    def _state_mask(self):
        """X11-style state bits for held buttons (Button1Mask = 0x0100 <<
        n-1) so bindings reading %s see a coherent chord."""
        s = 0
        for b in self._held:
            s |= 0x0100 << (b - 1)
        return s

    def _emit(self, w, seq, **kw):
        try:
            w.event_generate(seq, when="tail", **kw)
        except tk.TclError:
            pass

    def _inject(self, t, a, b, c):
        win = self.ov.win
        if t == EVT_MOVE:
            self._ev_client = (a, b)
            tx = min(max(0, a - self.dst[0]), self.ov.WIDTH - 1)
            ty = min(max(0, b - self.dst[1]), self.ov.HEIGHT - 1)
            self._ev_xy = (tx, ty)
            target = self._capture or self._widget_at(tx, ty) or win
            if self._capture is None:
                self._cross(target)
            x, y = self._rel(target, tx, ty)
            kw = dict(x=x, y=y, rootx=a, rooty=b,
                      state=self._state_mask())
            if self._held:
                # One <Bn-Motion> per held button, like real Tk does.
                for btn in sorted(self._held):
                    self._emit(target, "<B%d-Motion>" % btn, **kw)
            else:
                self._emit(target, "<Motion>", **kw)
            return
        if t in (EVT_BTNDN, EVT_BTNUP):
            btn = _IL_BTN.get(a, a + 1)
            tx, ty = self._ev_xy
            cx, cy = self._ev_client
            target = self._capture or self._widget_at(tx, ty) or win
            x, y = self._rel(target, tx, ty)
            seq = "<ButtonPress-%d>" % btn if t == EVT_BTNDN \
                else "<ButtonRelease-%d>" % btn
            self._emit(target, seq, x=x, y=y, rootx=cx, rooty=cy,
                       state=self._state_mask())
            if t == EVT_BTNDN:
                self._held.add(btn)
                self._capture = target        # implicit grab, like Tk
            else:
                self._held.discard(btn)
                if not self._held:
                    self._capture = None
            return
        if t == EVT_WHEEL:
            # a = notches (already /WHEEL_DELTA); c: 0=vert, 1=horiz. Tk
            # wants delta in WHEEL_DELTA units; a horizontal wheel scrolls
            # the same pane vertically (Tk has no hwheel binding).
            tx, ty = self._ev_xy
            cx, cy = self._ev_client
            target = self._hover or self._widget_at(tx, ty) or win
            x, y = self._rel(target, tx, ty)
            self._emit(target, "<MouseWheel>", x=x, y=y, rootx=cx,
                       rooty=cy, delta=a * 120)
            return
        if t in (EVT_KEYDN, EVT_KEYUP):
            ks = _VK_KEYSYM.get(a)
            if not ks:
                return
            target = win.focus_get() or win
            seq = "<KeyPress>" if t == EVT_KEYDN else "<KeyRelease>"
            self._emit(target, seq, keysym=ks, keycode=a,
                       state=self._state_mask())
            return
        if t == EVT_CHAR:
            try:
                ch = chr(a & 0x10FFFF)
            except (ValueError, OverflowError):
                return
            target = win.focus_get() or win
            kw = {"char": ch}
            ks = _VK_KEYSYM.get(a) or (ch if 32 <= a < 127 else None)
            if ks:
                kw["keysym"] = ks
            self._emit(target, "<KeyPress>", **kw)
            return

    def _cross(self, target):
        """<Leave>/<Enter> on hover change: cosmetic bindings (the close
        'x' highlight, hand cursors) still work on synthetic moves."""
        old = self._hover
        if old is target:
            return
        self._hover = target
        tx, ty = self._ev_xy
        cx, cy = self._ev_client
        for w, seq in ((old, "<Leave>"), (target, "<Enter>")):
            if w is None:
                continue
            try:
                x, y = self._rel(w, tx, ty)
            except tk.TclError:
                continue
            self._emit(w, seq, x=x, y=y, rootx=cx, rooty=cy)

    def _widget_at(self, tx, ty):
        """Deepest mapped descendant of the parked toplevel containing
        (tx, ty) in toplevel coords. Canvas-embedded windows are real Tk
        children (winfo_x/y is the scrolled position), so a plain tree
        walk reproduces Tk's own hit-test."""
        w = self.ov.win
        while True:
            hit = None
            try:
                children = w.winfo_children()
            except tk.TclError:
                return None
            # later siblings paint above earlier ones (Tk stacking) -
            # walk bottom-up so overlaps resolve to the topmost child
            for ch in reversed(children):
                try:
                    if not ch.winfo_ismapped():
                        continue
                    x, y = ch.winfo_x(), ch.winfo_y()
                    if x <= tx < x + ch.winfo_width() and \
                            y <= ty < y + ch.winfo_height():
                        hit = ch
                        break
                except tk.TclError:
                    continue
            if hit is None:
                return w
            tx -= hit.winfo_x()
            ty -= hit.winfo_y()
            w = hit

    def _rel(self, w, tx, ty):
        """Panel-local (tx, ty) -> coords relative to widget w."""
        x, y = tx, ty
        cur = w
        while cur is not None and cur != self.ov.win:
            x -= cur.winfo_x()
            y -= cur.winfo_y()
            cur = getattr(cur, "master", None)
        return x, y
