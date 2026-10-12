# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay - in-game control overlay (hotkey, default CTRL+ALT+G).
# FRAMED layout: a semi-transparent border BAND around the game client -
# left sidebar column with all controls + slim bottom status strip - with
# a clean see-through CENTER hole showing the game (live, or the frozen
# frame). The band hugs the game client rect (windowed) or the screen
# (exclusive fullscreen); a ~500 ms poll re-fits on resize/move.
# Never steals focus, never minimizes the game: WS_EX_NOACTIVATE +
# SW_SHOWNOACTIVATE ordering, plus a WM_MOUSEACTIVATE->MA_NOACTIVATE
# wndproc subclass so clicks on the band operate the controls without
# activating the window; the hole pixels pass straight through.
# External mode uses -transparentcolor CHROMA for the click-through hole;
# in-layer mode publishes the same rect as a bitmap hole (overlay_shm).
#
# Every overlay decision lands in logs\overlay.log (overlay_log) - the
# on_log widget alone left field reports traceless.
#
# MOUSE & FOCUS (Game Bar style):
# The panel appears topmost over the game WITHOUT taking the foreground -
# no activation, no minimize, no flash (WS_EX_NOACTIVATE + deiconify only;
# verified against GetForegroundWindow by _probe_noactivate.py). Foreground
# stealing is OPT-IN via cfg overlay_focus: only then does the overlay run
# _force_foreground/focus_force and return focus on hide. Tk takefocus is
# stripped from the widget tree while focus mode is off so a click can never
# route into Tk's focus command (focus_set activates the toplevel -> steals
# the game's foreground -> exclusive-fullscreen games minimize).
#
# NON-ACTIVATION INVARIANT (exclusive-fullscreen safe open path):
#   1. WS_EX_NOACTIVATE is applied to the top-level HWND in __init__ while
#      the window is still WITHDRAWN and again at the top of show() BEFORE
#      anything that could map the window - a transient map without the
#      style is the only way the open path can steal the game's foreground.
#   2. show() never calls focus_set/focus_force/SetForegroundWindow unless
#      cfg overlay_focus is on; Tk deiconify/lift/-topmost are followed by an
#      explicit ShowWindow(SW_SHOWNOACTIVATE) + SetWindowPos(NOACTIVATE) so
#      the displayed state never depends on Tk internals honoring the style.
#   3. hide() restores foreground to the game ONLY when this session took it
#      (focus mode); otherwise hide() never touches the game window.
#   4. Clicks: takefocus is stripped from the whole widget tree so a mouse
#      click can never route into Tk focus -> toplevel activation.
# Live evidence: every show() logs "[overlay] shown ... exstyle=0x..
# fg_before/fg_after" (sampled ~50 ms after the map).
#
# PHOTO MODE (opt-in, cfg overlay_freeze): open the overlay -> the frame
# FREEZES (the layer/proxy holds
# the raw frame and re-blits its processed result, the game keeps running
# underneath). Turn the gain/blend/scale/sharpness knob -> knob push +
# reprocess flag bump -> the SAME original frame is reprocessed and the frozen
# picture updates with live results.
# Close -> the freeze flag drops, the game continues with the new settings.
#
# OVERLAY MODES (cfg overlay_mode):
#   "external" - the classic real toplevel mapped on top of the game
#     (everything in the paragraphs above; today's behaviour, kept as the
#     fallback for hosts without the shm-aware layer).
#   "inlayer"  - DEFAULT. Steam-style: the toplevel stays MAPPED, on-screen
#     but invisible (-alpha 0 + WS_EX_TRANSPARENT click-through - an
#     off-screen window has an empty GDI visible region and Tk's paint is
#     clipped away), so Tk keeps laying out/painting and every binding
#     keeps working; a publish pump PrintWindow's it into
#     "Local\DLSS5_UI_<pid>" (~15 Hz + heartbeat) and the in-process Vulkan
#     layer alpha-blends it into the frame. While ui_visible is set the
#     layer's wndproc filter EATS the game's window input and posts it to
#     the shared event ring; a ~10 ms Tk-side pump turns those events back
#     into Tk events on the panel widgets - hit-testing comes for free
#     from the widget tree. In-layer draws the cursor sprite itself (and
#     eats the game's WM_SETCURSOR) instead of showing an OS cursor; the
#     input guard (overlay_input.py) still runs in BOTH modes - a game
#     that ClipCursor-pins the pointer would starve the wndproc of the
#     very input it is supposed to route. Only the OS-cursor shaping
#     (ShowCursor/SetCursor over our own window) is external-only.
#
# Threading: the overlay never does I/O itself. It renders ctl.snapshot()
# (monitor thread) and routes every action through ctl.submit() (worker).
# The in-layer shm pumps are plain after() timers on the Tk thread -
# PrintWindow is a synchronous cross-thread WM_PRINT send, so it cannot
# live on a worker thread anyway (see InLayerUI).
# ============================================================================
import ctypes
import os
import time
import tkinter as tk
from collections import deque
from ctypes import wintypes

from .processes import hwnd_for_pid
from . import gamelaunch
from .i18n import (tr, scale_preset_labels, scale_to_preset_label,
                   preset_label_to_scale)
from .overlay_hotkey import (VK, POLL_MS, parse_hotkey,  # noqa: F401
                             hotkey_label, HotkeyPoller)
from .overlay_inlayer import InLayerUI
from .overlay_modal import build_modal_panel, ExternalCursor
from .overlay_input import OverlayInputGuard
from .overlay_widgets import (build_panel, BG, PANEL, PANEL2, FG, MUTED,
                              ACCENT, GREEN, RED, AMBER,
                              CHROMA, SIDEBAR_W, STRIP_H)
from .overlay_shm import MAX_W as SHM_MAX_W, MAX_H as SHM_MAX_H
from .overlay_win32 import (_u32, GWL_EXSTYLE, WS_EX_TOPMOST,  # noqa: F401
                            WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
                            HWND_TOPMOST, SWP_NOMOVE, SWP_NOSIZE,
                            SWP_NOACTIVATE,
                            _set_noactivate, _free_cursor, _input_jiggle,
                            _force_foreground, _toplevel_hwnd, _exstyle,
                            _show_noactivate,
                            set_clickthrough, subclass_noactivate,
                            client_rect_screen, primary_work_area,
                            monitor_work_area_for_hwnd)
from . import overlay_log

VK_LBUTTON = 0x01
DEBOUNCE_MS = 250
STATUS_MS = 500
SCALE_APPLY_S = 6.5    # consumer-side engine rebuild window after a scale
                       # push (~4-6 s; mirrors controller._REPROC_LATE_S)
FIT_MS = 500           # game-rect poll cadence while shown (resize/move)
CURSOR_KEEP_MS = 32    # jail poll cadence (fullscreen click-escape guard)
TOPMOST_REASSERT_S = 1.0


class ControlOverlay:
    """Frameless always-on-top control panel floating over the game."""

    WIDTH, HEIGHT = 470, 710

    def __init__(self, master, ctl, on_log):
        self.ctl = ctl
        self.on_log = on_log
        self.win = tk.Toplevel(master)
        self.win.overrideredirect(True)          # frameless: no OS window look
        self.win.attributes("-topmost", True)
        # cfg overlay_style: "modal" (default) = a centered Steam-style
        # panel on a dimmed full-client backdrop - the shm hole stays
        # degenerate so ALL client input routes to the ring for the whole
        # visible session; "band" = the legacy framed sidebar+strip with a
        # see-through center hole. Style is read once at build time.
        self._modal = (str(ctl.cfg.get("overlay_style") or "modal")
                       != "band")
        # Modal: a single -alpha is the only knob Tk offers, so it has to
        # cover two duties - solid panel AND dim backdrop. 0.95 keeps the
        # panel effectively opaque (a bright game frame ghosted through
        # at 0.85) while the dark backdrop still reads as a scrim.
        self._alpha = 0.95 if self._modal else 0.88
        self.win.attributes("-alpha", self._alpha)
        self.win.configure(bg=BG, cursor="arrow")
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.hide)
        self.win.withdraw()
        # NON-ACTIVATION INVARIANT (1): the top-level HWND gets
        # WS_EX_NOACTIVATE NOW, while the window is still withdrawn, so no
        # later map (deiconify, geometry replay, Tk internals) can ever see
        # an activatable window - even for a single frame.
        hwnd0 = _toplevel_hwnd(self.win)
        if hwnd0:
            _set_noactivate(hwnd0, True)
        self._after_id = None
        self._freeze_flags = False   # primary freeze: layer/proxy flags
        self._pending_push = None    # (value, time) of an in-flight push
        self._after_id_blend = None  # debounce timer for the blend knob
        self._after_id_sharpness = None  # debounce timer for sharpness
        self._after_id_scale = None  # debounce timer for the scale preset
        self._scale_apply_id = None      # apply-window end timer (scale)
        self._scale_apply_deadline = 0.0  # monotonic rebuild ETA (~6 s)
        self._scale_queued = False       # a pick landed mid-rebuild
        self._game_hwnd = None       # foreground restore target
        self._focus_mode = False     # we took the foreground this session
        self._jail_cursor = True     # recompute per show(): fullscreen only
        self._tf_saved = None        # original takefocus per widget
        self._dragging = False       # a mouse button is held over the panel
        self._last_topmost = 0.0     # last HWND_TOPMOST re-assert timestamp
        self._hwnd = None            # cached top-level HWND (per show())
        self._inlayer = False        # this session composites via the layer
        self._ui_visible = False     # logical panel visibility (in-layer)
        self._il = None              # InLayerUI bridge, built on first arm
        self._shm_failed_pid = None  # shm open failed / no layer consumer
                                     # for this pid -> external for the
                                     # rest of that session
        self._combo_tags_saved = None  # combobox bindtags while in-layer
        # Input lane (overlay_input.py): while the overlay is VISIBLE the
        # guard re-releases the game's ClipCursor/SetCapture grip (~180 ms
        # watchdog - games re-arm per frame, a show() one-shot loses) and
        # raises the cursor for external mode; in-layer mode releases the
        # confinement only, the layer-drawn sprite is the cursor there.
        self._input_guard = OverlayInputGuard(self)
        self._extent = (self.WIDTH, self.HEIGHT)  # current panel extent
        self._rect = None            # last applied (x,y,w,h) screen rect
        self._hole = (0, 0, 0, 0)    # transparent center, bitmap coords
        self._fit_id = None          # game-rect poll timer
        self._last_fit_log = None    # dedup for the fit log line
        self._entry_kbd = False      # a text widget holds Tk focus
        self._kbd_session = False    # modal: kbd_capture for the whole
                                     # visible session (ESC = close,
                                     # keys never leak to the game)
        self._opaque = None          # modal: panel rect in bitmap coords
                                     # (backdrop pixels publish at ~55%
                                     # alpha); None = uniform alpha
        self._extcursor = None       # external-modal own cursor window
        self._esc_id = None          # ESC edge poll (external + belt)
        self._esc_was = False
        self._log_tail = deque(maxlen=6)   # System tab's log tail
        self.win.bind("<Escape>", lambda _e: self._esc_hide())
        # Focus tracking for the shm kbd_capture flag (bit2): only while
        # an Entry-like widget in OUR toplevel holds focus does the layer
        # route key events to the ring instead of the game. bind_all sees
        # the manager's widgets too - the winfo_toplevel() check inside
        # confines it to this overlay.
        self.win.bind_all("<FocusIn>", self._focus_changed, add="+")
        self.win.bind_all("<FocusOut>", self._focus_changed, add="+")
        for cls in ("Entry", "TEntry", "Spinbox", "Text"):
            self.win.bind_class(cls, "<ButtonPress-1>",
                                self._entry_click, add="+")
        # Drag tracking feeds _cursor_keeper: while a button is held over
        # the panel the keeper must not ReleaseCapture mid-gesture. The
        # toplevel bindtag sees presses/releases from every descendant.
        self.win.bind("<ButtonPress>", self._button_press, add="+")
        self.win.bind("<ButtonRelease>", self._button_release, add="+")
        self._build()
        self.poller = HotkeyPoller(ctl.cfg.get("overlay_hotkey"), self.toggle)
        self.poller.attach(master)

    # ------------------------------------------------------------- build ---
    def _build(self):
        # Widget-tree construction is delegated: overlay_widgets
        # .build_panel (legacy framed band) or overlay_modal
        # .build_modal_panel (centered Steam-style card). Both assign
        # every widget/var straight onto this instance under the same
        # attribute names.
        if self._modal:
            build_modal_panel(self)
        else:
            build_panel(self)

    def _log(self, msg):
        """Overlay decisions go to BOTH sinks: the UI widget (on_log) and
        logs\\overlay.log (field reports read files, not widgets). The
        modal style additionally shows the last few lines in the System
        tab."""
        overlay_log.log(msg)
        self._log_tail.append(msg)
        lbl = getattr(self, "syslog_lbl", None)
        if lbl is not None:
            try:
                lbl.config(text="\n".join(self._log_tail))
            except tk.TclError:
                pass
        try:
            self.on_log(msg)
        except Exception:
            pass

    # --------------------------------------------------- fit to the game --
    def _game_rect(self):
        """(x, y, w, h) screen rect the framed band hugs: the game CLIENT
        rect (windowed: real play area; borderless/exclusive fullscreen:
        the display mode = screen). Falls back to the primary monitor
        work area when no game hwnd is known."""
        gh = self._game_hwnd
        # Re-resolve the pid's biggest top-level every query: GTA IV-style
        # games own several top-levels and re-parent/resize them across
        # mode switches, so a hwnd cached at show time can go stale or
        # stop being the render window (the layer's ui_hwnd follows the
        # guarded surface window; the biggest-top-level heuristic is the
        # closest user-mode proxy). Adopting the fresh pick keeps the fit
        # poll converging onto the real client instead of pinning a
        # transient 200x200 splash / work-area fallback forever.
        pid = None
        try:
            pid = self.ctl.active_game_pid()
        except Exception:
            pid = None
        if pid:
            gh2 = hwnd_for_pid(pid)
            if gh2 and gh2 != gh:
                self._game_hwnd = gh2
                self._log("[overlay] game hwnd re-resolved 0x%x -> 0x%x"
                          % (gh or 0, gh2))
                gh = gh2
        if gh:
            try:
                rect = client_rect_screen(gh)
            except Exception:
                rect = None
            if rect:
                return rect
        wa = primary_work_area()
        if wa:
            return (wa[0], wa[1], wa[2] - wa[0], wa[3] - wa[1])
        try:
            return (0, 0, self.win.winfo_screenwidth(),
                    self.win.winfo_screenheight())
        except tk.TclError:
            return (0, 0, 800, 600)

    def _recompute_hole(self):
        """Hole rect in BITMAP coords from the LIVE widget geometry:
        (sidebar_w, 0, W, H - strip_h). Called after geometry/layout so a
        packed band of any size stays truthful. The modal style is
        intentionally hole-free: the whole client-sized bitmap is UI, so
        every client pixel routes input to the ring (the layer's cursor
        sprite is then visible over the whole client) and the backdrop
        dim reaches every pixel."""
        if self._modal:
            self._hole = (0, 0, 0, 0)
            return
        W, H = self._extent
        try:
            sw = self.sidebar.winfo_width() or SIDEBAR_W
            sh = self.strip.winfo_height() or STRIP_H
        except tk.TclError:
            sw, sh = SIDEBAR_W, STRIP_H
        if W > sw and H > sh:
            self._hole = (sw, 0, W, H - sh)
        else:
            self._hole = (0, 0, 0, 0)    # client too small: all band

    def _fit_geometry(self, force=False):
        """Size/position the band to the game client rect. Runs on show
        (force) and on the FIT_MS poll (debounced: identical rects skip
        the geometry write, so a stable game costs one compare - but the
        extent/hole refresh below always runs, so a just-mapped window
        fixes its live metrics on the first tick)."""
        rect = self._game_rect()
        x, y, w, h = rect
        if force or rect != self._rect:
            self._rect = rect
            if self._inlayer:
                # The shm bitmap cap: a client wider than the cap gets
                # the band clamped to the top-left cap rect (far edges
                # uncovered - logged once per size, still functional).
                pw, ph = min(w, SHM_MAX_W), min(h, SHM_MAX_H)
                if (pw, ph) != (w, h):
                    self._log("[overlay] game client %dx%d exceeds shm "
                              "cap %dx%d - band clamped top-left" % (
                                  w, h, SHM_MAX_W, SHM_MAX_H))
                # The invisible window parks at the client origin so
                # widget coords == client coords. A minimized game
                # reports a -32000 origin - off-screen paint is clipped
                # to nothing, so clamp the park into the virtual desktop.
                sx, sy = x, y
                l, t, r, b = self._vroot_bounds()
                if sx + pw <= l or sx >= r or sy + ph <= t or sy >= b:
                    sx, sy = l + 8, t + 8
                self.win.geometry("%dx%d+%d+%d" % (pw, ph, sx, sy))
                self._il.fit(x, y, pw, ph)
            else:
                # A minimized/hidden game reports a -32000-ish origin:
                # an off-screen window would never be seen, so drop the
                # band onto the primary work area until the game returns.
                l, t, r, b = self._vroot_bounds()
                if x + w <= l or x >= r or y + h <= t or y >= b:
                    wa = primary_work_area() or (l, t, r, b)
                    x, y = wa[0], wa[1]
                    w, h = wa[2] - wa[0], wa[3] - wa[1]
                self.win.geometry("%dx%d+%d+%d" % (w, h, x, y))
        try:
            self.win.update_idletasks()
            if self.win.winfo_viewable():
                self._extent = (self.win.winfo_width(),
                                self.win.winfo_height())
            else:
                # Unmapped yet (external show runs fit BEFORE deiconify):
                # geometry() already set the requested size.
                self._extent = (self.win.winfo_reqwidth(),
                                self.win.winfo_reqheight())
        except tk.TclError:
            self._extent = (w, h)
        # Instance shadow: every WIDTH/HEIGHT reader sees live extent.
        self.WIDTH, self.HEIGHT = self._extent
        self._place_panel()
        self._recompute_hole()
        if force or rect != getattr(self, "_last_fit_log", None):
            self._last_fit_log = rect
            self._log("[overlay] fit rect=%s extent=%s hole=%s inlayer=%d"
                      % (rect, self._extent, self._hole, self._inlayer))

    def _place_panel(self):
        """Modal style: (re)center the panel inside the live extent and
        record the opaque rect the publisher uses to split panel pixels
        (full alpha) from backdrop pixels (~55% dim scrim). The rect is
        in toplevel == bitmap coords."""
        if not self._modal:
            return
        W, H = self._extent
        pw = min(880, max(200, W - 40))
        ph = min(600, max(160, H - 40))
        pw = min(pw, W)
        ph = min(ph, H)
        self._opaque = ((W - pw) // 2, (H - ph) // 2,
                        (W + pw) // 2, (H + ph) // 2)
        try:
            # relx/rely/anchor are passed explicitly - place() merges
            # unmentioned options with the previous config, so a stale
            # centering anchor would silently offset the rect.
            self.modal_panel.place(x=self._opaque[0], y=self._opaque[1],
                                   width=pw, height=ph,
                                   relx=0, rely=0, anchor="nw")
        except tk.TclError:
            pass

    # -------------------------------------------------- modal tab pages ---
    def _tab_show(self, key):
        """Modal: switch the content page + nav highlight; the wheel
        proxy follows the page's own scroll canvas (if any)."""
        if not self._modal or key not in self._pages:
            return
        self._tab = key
        for k, page in self._pages.items():
            try:
                page.pack_forget()
            except tk.TclError:
                pass
        try:
            self._pages[key].pack(fill="both", expand=True)
        except tk.TclError:
            pass
        for k, btn in self._nav_btns.items():
            try:
                btn.config(bg=ACCENT if k == key else PANEL2,
                           fg="#ffffff" if k == key else FG)
            except tk.TclError:
                pass
        self.body_canvas.target = self._page_canvas.get(key)

    def _preset_pick(self, label):
        """Modal preset button: same debounce/queue path as the band's
        combobox - the settled selection is what rebuilds the engine."""
        self.scale_var.set(label)
        self._preset_refresh()
        self._scale_selected()

    def _preset_refresh(self):
        """Highlight the picked preset button (ACCENT); the applied one
        is still named by scale_state's 'applied preset:' line."""
        if not self._modal:
            return
        try:
            pick = self.scale_var.get()
        except tk.TclError:
            return
        for lbl, b in getattr(self, "preset_btns", {}).items():
            try:
                on = lbl == pick
                bg = ACCENT if on else PANEL2
                b.config(bg=bg)
                kids = b.winfo_children()
                if kids:
                    # Two-line segment cell: name stays readable, the
                    # pct line dims when inactive.
                    for ch in kids:
                        ch.config(bg=bg,
                                  fg=("#ffffff" if on
                                      else (MUTED
                                            if ch is getattr(
                                                b, "_pct_lbl", None)
                                            else FG)))
                else:
                    # Legacy flat preset button.
                    b.config(fg="#ffffff" if on else FG)
            except tk.TclError:
                pass

    def _fg_stats_line(self):
        """Last 'fg stats' line of the layer log for the Frame gen tab
        (the same tail the manager log reads; ~64 KB read per tick)."""
        try:
            p = gamelaunch.LAYER_LOG
            size = os.path.getsize(p)
            with open(p, "rb") as f:
                f.seek(max(0, size - 65536))
                data = f.read()
            at = data.rfind(b"fg stats")
            if at < 0:
                return ""
            return data[at:].split(b"\n", 1)[0].decode(
                "ascii", "replace").strip()[:160]
        except OSError:
            return ""

    def _modal_refresh(self):
        """Modal-only header/tab refreshes driven from _refresh."""
        snap = self.ctl.snapshot()
        active_game = snap.get("active_game")
        try:
            self.game_lbl.config(
                text=os.path.basename(active_game) if active_game else "")
            self.preset_lbl.config(
                text=scale_to_preset_label(self.ctl.cfg.get("scale")))
            if self._pending_push is None:
                self.gain_val.config(
                    text="%.2f" % round(float(self.var.get()), 3))
            self.fg_stats_lbl.config(text=self._fg_stats_line())
        except tk.TclError:
            pass

    # ------------------------------------------------------------- ESC ---
    def _esc_hide(self):
        """<Escape> (Tk binding; in-layer key events arrive via the ring):
        close the overlay. Guarded so the async-state poll's double-fire
        cannot run hide() twice."""
        if self._is_shown():
            self.hide()

    def _start_esc(self):
        """While shown, poll the physical ESC state ~80 ms: the in-layer
        ring covers ESC, but an un-eaten key (dead ring, external mode
        where our never-focused toplevel sees no OS keys at all) must
        still close the modal. Edge-triggered; armed with the CURRENT
        state so the keypress that opened the overlay cannot insta-close
        it."""
        self._cancel_esc()
        try:
            self._esc_was = bool(_u32.GetAsyncKeyState(0x1B) & 0x8000)
            self._esc_id = self.win.after(80, self._esc_tick)
        except tk.TclError:
            pass

    def _cancel_esc(self):
        if self._esc_id is not None:
            try:
                self.win.after_cancel(self._esc_id)
            except Exception:
                pass
            self._esc_id = None

    def _esc_tick(self):
        self._esc_id = None
        if not self._is_shown():
            return
        try:
            down = bool(_u32.GetAsyncKeyState(0x1B) & 0x8000)
        except Exception:
            down = False
        if down and not self._esc_was:
            self._esc_was = True
            self.hide()
            return
        self._esc_was = down
        try:
            self._esc_id = self.win.after(80, self._esc_tick)
        except tk.TclError:
            pass

    def _fit_tick(self):
        """~500 ms poll: track the game client rect (move/resize/mode
        switch) and re-fit the band. Self-terminates when hidden."""
        self._fit_id = None
        if not self._is_shown():
            return
        try:
            self._fit_geometry()
        except Exception as e:
            self._log("[overlay] fit error: %s" % e)
        try:
            self._fit_id = self.win.after(FIT_MS, self._fit_tick)
        except tk.TclError:
            pass

    def _cancel_fit(self):
        if self._fit_id is not None:
            try:
                self.win.after_cancel(self._fit_id)
            except Exception:
                pass
            self._fit_id = None

    def _start_fit(self):
        self._cancel_fit()
        try:
            self._fit_id = self.win.after(FIT_MS, self._fit_tick)
        except tk.TclError:
            pass

    # ----------------------------------------------------- entry focus ---
    def _entry_like(self, w):
        """Text-input widget check for the kbd_capture flag contract."""
        try:
            return w.winfo_class() in ("Entry", "TEntry", "Spinbox", "Text")
        except tk.TclError:
            return False

    def _focus_changed(self, _e=None):
        """FocusIn/FocusOut (bind_all): set/clear the shm kbd_capture flag
        so the layer routes key events to the ring only while an entry
        inside OUR overlay holds focus."""
        try:
            w = self.win.focus_get()
        except (tk.TclError, KeyError):
            # KeyError: focus sits on a Tk-untracked window (ttk
            # 'popdown', foreign widget) - nametowidget can't resolve it.
            w = None
        if w is not None:
            try:
                if w.winfo_toplevel() != self.win:
                    w = None
            except tk.TclError:
                w = None
        on = bool(w is not None and self._entry_like(w)) \
            or self._kbd_session
        if on == self._entry_kbd:
            return
        self._entry_kbd = on
        self._log("[overlay] kbd_capture %s (focus widget %s)" % (
            "on" if on else "off", w))
        if self._il and self._il.is_armed():
            self._il.shm.set_kbd_capture(on)

    def _entry_click(self, e):
        """Click on a text widget: focus follows the click, best-effort.
        In-layer the toplevel is WS_EX_NOACTIVATE so focus_set cannot
        become a steal; external non-focus mode skips it (a real steal
        is exactly what we exist to avoid)."""
        if self._inlayer or bool(self.ctl.cfg.get("overlay_focus")):
            try:
                e.widget.focus_set()
            except tk.TclError:
                pass

    # -------------------------------------------------------------- drag ---
    def _button_press(self, _e):
        """Any button held over the panel - _cursor_keeper must not run
        ReleaseCapture/SetCursor mid-gesture (kills Tk slider drags)."""
        self._dragging = True

    def _button_release(self, _e):
        self._dragging = False

    def _vroot_bounds(self):
        """(l, t, r, b) of the virtual desktop spanning ALL monitors -
        winfo_screenwidth() is the primary monitor only. Falls back to the
        primary screen when the vroot query fails."""
        try:
            l, t = self.win.winfo_vrootx(), self.win.winfo_vrooty()
            w, h = (self.win.winfo_vrootwidth(),
                    self.win.winfo_vrootheight())
            if w > 0 and h > 0:
                return (l, t, l + w, t + h)
        except tk.TclError:
            pass
        return (0, 0, self.win.winfo_screenwidth(),
                self.win.winfo_screenheight())

    # Drag-to-move was removed: the band hugs the game client edges, and a
    # dragged position would contradict the auto-fit contract.

    # ----------------------------------------------------------- actions ---
    def _toggle_processing(self):
        self.ctl.submit(self.ctl.processing_toggle)

    def _debounced_push(self, _val):
        g = round(float(self.var.get()), 3)
        if self._suppress_scale is not None and \
                abs(g - self._suppress_scale) < 1e-9:
            self._suppress_scale = None      # echo of a status-driven sync
            return
        self._suppress_scale = None
        self._pending_push = (g, time.time())
        if self._after_id:
            self.win.after_cancel(self._after_id)
        self._after_id = self.win.after(DEBOUNCE_MS, self._push)

    def _push(self):
        self._after_id = None
        g = round(float(self.var.get()), 3)
        self.ctl.submit(self.ctl.set_gain, g)

    def _reset_gain(self):
        self.var.set(1.0)
        self._debounced_push(1.0)

    def _on_phase30_change(self, key, val):
        self.ctl.submit(self.ctl.set_phase30, {key: val})
        self._log("[overlay] Phase30 %s -> %s" % (key, val))

    def _on_phase30_reset(self):
        self.ctl.submit(self.ctl.reset_phase30)
        self._log("[overlay] Phase30 reset to defaults")

    def sync_phase30(self, p):
        if hasattr(self, "p30_panel") and self.p30_panel:
            self.p30_panel.set_values(p)

    def _debounced_blend(self, _val):
        b = round(float(self.blend_var.get()), 3)
        self.blend_val.config(text="%.2f" % b)
        if self._after_id_blend:
            self.win.after_cancel(self._after_id_blend)
        self._after_id_blend = self.win.after(DEBOUNCE_MS, self._push_blend)

    def _push_blend(self):
        self._after_id_blend = None
        b = round(float(self.blend_var.get()), 3)
        self.ctl.submit(self.ctl.set_blend, b)

    def _reset_blend(self):
        self.blend_var.set(1.0)
        self._debounced_blend(1.0)

    def _scale_selected(self, _e=None):
        """Scale preset pick. In-layer the combo cycles on each click, so a
        burst of clicks used to queue a multi-second engine rebuild PER
        value (daemon log: ~24 re-inits for one choice - the screen kept
        serving the old scale for tens of seconds and read as "preset does
        not apply"). Debounce: only the settled selection is pushed, and a
        pick landing mid-rebuild is QUEUED until the in-flight one lands -
        one trailing rebuild, never one per click."""
        lbl = self.scale_var.get()
        if self._scale_applying():
            if not self._scale_queued:
                self._scale_queued = True
                self._log("[overlay] scale pick %s queued - engine "
                          "rebuild in flight" % lbl)
            self._scale_state_update()
            return
        if self._after_id_scale:
            self.win.after_cancel(self._after_id_scale)
        self._after_id_scale = self.win.after(600, self._push_scale)
        self._scale_state_update()
        self._log("[overlay] scale selected: %s (applying on settle)" % lbl)

    def _push_scale(self):
        self._after_id_scale = None
        lbl = self.scale_var.get()
        s = preset_label_to_scale(lbl)
        self.ctl.submit(self.ctl.set_scale, s)
        self._log("[overlay] scale set to %.2f (%s)" % (s, lbl))
        self._scale_apply_start()

    # ---- preset apply window (the ~6 s consumer-side engine rebuild) ----
    # cfg is already written by ctl.set_scale; the rebuild itself happens
    # inside the layer/daemon where m13 cannot observe progress. The wall
    # clock is the only honest "applied" signal, so the window is fixed.
    def _scale_applying(self):
        return time.monotonic() < self._scale_apply_deadline

    def _scale_apply_start(self):
        """Open the rebuild window after a pushed scale value."""
        self._scale_apply_deadline = time.monotonic() + SCALE_APPLY_S
        if self._scale_apply_id is not None:
            try:
                self.win.after_cancel(self._scale_apply_id)
            except Exception:
                pass
        self._scale_apply_id = self.win.after(
            int(SCALE_APPLY_S * 1000), self._scale_apply_done)
        self._scale_state_update()

    def _scale_apply_done(self):
        self._scale_apply_id = None
        if self._scale_queued:
            # Rebuild landed and a newer pick was queued meanwhile: push
            # ONLY the settled final value (one rebuild, not one/click).
            self._scale_queued = False
            self._push_scale()
            return
        self._scale_state_update()

    def _scale_state_update(self):
        """Refresh the line under the preset combo: the APPLIED preset at
        rest (full name, even if the combobox would clip it), 'applying'
        while a debounce/rebuild runs, and 'queued' for a deferred pick."""
        try:
            pick = self.scale_var.get()
            applied = scale_to_preset_label(self.ctl.cfg.get("scale"))
            if self._after_id_scale:
                txt = tr("ov_scale_applying", pick)
                self.scale_state.config(text=txt, fg=AMBER)
            elif self._scale_applying():
                txt = tr("ov_scale_applying", applied)
                if self._scale_queued and pick != applied:
                    txt += "  " + tr("ov_scale_queued", pick)
                self.scale_state.config(text=txt, fg=AMBER)
            else:
                self.scale_state.config(
                    text=tr("ov_scale_current", applied), fg=MUTED)
        except tk.TclError:
            pass
        self._preset_refresh()

    def _save_framegen(self):
        fg = bool(self.fg_var.get())
        self.ctl.submit(self.ctl.set_framegen, fg)
        self._log("[overlay] frame generation %s" % (
            "enabled" if fg else "disabled"))

    def _debounced_sharpness(self, _val):
        s = round(float(self.sharpness_var.get()), 2)
        self.sharpness_val.config(text="%.2f" % s)
        if self._after_id_sharpness:
            self.win.after_cancel(self._after_id_sharpness)
        self._after_id_sharpness = self.win.after(
            DEBOUNCE_MS, self._push_sharpness)

    def _push_sharpness(self):
        self._after_id_sharpness = None
        s = round(float(self.sharpness_var.get()), 2)
        self.ctl.submit(self.ctl.set_sharpness, s)

    def _reset_sharpness(self):
        self.sharpness_var.set(0.8)
        self._debounced_sharpness(0.8)

    def sync_scale(self, s):
        """Manager-side scale change mirror: seed the combo to the pushed
        value and open the same apply window - the consumer runs the same
        ~6 s rebuild no matter which UI pushed the knob."""
        if self._after_id_scale:
            try:
                self.win.after_cancel(self._after_id_scale)
            except Exception:
                pass
            self._after_id_scale = None
        self.scale_var.set(scale_to_preset_label(s))
        self._scale_apply_start()

    def sync_framegen(self, fg):
        self.fg_var.set(bool(fg))

    def sync_sharpness(self, s):
        self.sharpness_var.set(float(s))
        self.sharpness_val.config(text="%.2f" % float(s))

    def _save_autopause(self):
        self.ctl.cfg.set("overlay_autopause", bool(self.ap_var.get()))

    def _save_freeze(self):
        val = bool(self.fr_var.get())
        self.ctl.cfg.set("overlay_freeze", val)
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        game_mode = mode in ("dx9", "dx11", "dx12", "vulkan")
        if val and game_mode:
            if self.ctl.freeze_supported():
                gamelaunch.freeze_set(True)
                self._freeze_flags = True
                self.freeze_lbl.config(text=tr("ov_freeze_on"), fg=AMBER)
                self._log("[overlay] frame frozen (flags); knob turns reprocess the original")
        else:
            self.unfreeze()
            self.freeze_lbl.config(text="")

    # ------------------------------------------------------------ status ---
    def set_status(self, text):
        try:
            self.status.config(text=text)
        except tk.TclError:
            pass

    def _refresh(self):
        """Render the monitor snapshot; runs only while visible."""
        if not self._is_shown():
            return
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        names = {"dx9": tr("ov_game_dx9"), "dx11": tr("ov_game_dx11"),
                 "dx12": tr("ov_game_dx12"), "vulkan": tr("ov_game_vulkan"),
                 "screen": tr("ov_screen")}
        # Processing state: file flags (manager toggles + the m12 proxy's
        # own CTRL+ALT+X flag flips) UNION the consumer's hotkey state the
        # monitor reads off its log tail (layer CTRL+ALT+X/Q, m12 detach).
        # The big button shows state+action; the header pill is the
        # always-on readout - the in-game feedback for CTRL+ALT+X/Q.
        pstate = self.ctl.processing_state(snap)
        if pstate == "paused":
            self.toggle_btn.config(text=tr("ov_proc_paused"), bg=GREEN,
                                   activebackground="#2ea043")
        elif pstate == "on":
            self.toggle_btn.config(text=tr("ov_proc_on"), bg=ACCENT,
                                   activebackground="#3a70d6")
        elif pstate == "off":
            self.toggle_btn.config(text=tr("ov_proc_off"), bg=PANEL,
                                   activebackground=PANEL2)
        else:
            self.toggle_btn.config(text=tr("ov_proc_none"), bg=PANEL,
                                   activebackground=PANEL2)
        state_key = "ov_state_" + pstate if pstate else None
        self.state_lbl.config(
            text=tr(state_key) if state_key else "--",
            fg={"on": GREEN, "paused": AMBER, "off": RED}.get(
                pstate, MUTED))
        self.mode_lbl.config(text=tr("ov_target", names.get(
            mode, tr("ov_target_none"))))
        active_game = snap.get("active_game")
        game_meta = (snap.get("games") or {}).get(active_game) if active_game else None
        game_arch = game_meta.get("arch") if game_meta else None

        if snap.get("daemon_pid") is not None:
            g, f = snap.get("daemon_gain"), snap.get("daemon_frames")
            txt = tr("ov_status_daemon") + " (PID %s)" % snap["daemon_pid"]
            if g is not None:
                txt += tr("ov_daemon_stats", g, f or 0)
                pending = self._pending_push
                stale_push = pending and (time.time() - pending[1] > 2.0
                                          or abs(pending[0] - g) < 0.001)
                if stale_push:
                    self._pending_push = None
                if not self._scale_dragging and not self._pending_push:
                    self._suppress_scale = round(g, 3)
                    self.var.set(g)
            if snap.get("daemon_err"):
                txt += tr("daemon_busy")
            self.status.config(text=txt, fg=GREEN)
        elif mode in ("vulkan", "dx12", "dx11") or (active_game and game_arch != "x86"):
            target_str = names.get(mode, mode or "Direct")
            self.status.config(
                text="%s (%s, 0 ms lag)" % (tr("ov_status_direct"), target_str),
                fg=GREEN)
        elif snap.get("screen_pid") is not None:
            self.status.config(text=tr("ov_screen_up", snap["screen_pid"]),
                               fg=ACCENT)
        elif (active_game and game_arch == "x86") or (mode == "dx9" and snap.get("daemon_pid") is None):
            self.status.config(text=tr("ov_status_daemon_down_x86"), fg=RED)
        else:
            self.status.config(text=tr("ov_status_standby"), fg=MUTED)

        p30 = snap.get("phase30")
        if p30 and hasattr(self, "p30_panel") and self.p30_panel and not self.p30_panel.is_dragging():
            self.p30_panel.set_values(p30)

        if self._modal:
            self._modal_refresh()
        if not self._inlayer:
            # external-mode z-order insurance only; an off-screen parked
            # toplevel has nothing to re-assert.
            self._reassert_topmost()
        self.win.after(STATUS_MS, self._refresh)

    def _reassert_topmost(self):
        """~1/s topmost insurance while visible: another always-on-top
        window (GPU OSD, another game bar) can cover the panel. A bare
        HWND_TOPMOST restack with SWP_NOACTIVATE is pure z-order - it can
        never become an activation."""
        now = time.time()
        if now - self._last_topmost < TOPMOST_REASSERT_S:
            return
        self._last_topmost = now
        hwnd = self._hwnd
        if not hwnd:
            return
        try:
            if _u32.IsWindowVisible(hwnd):
                _u32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except (ValueError, OSError):
            pass


    # ------------------------------------------------------- focus / cursor
    def _take_focus(self):
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        _free_cursor(self._game_hwnd)
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        self.win.lift()
        if bool(self.ctl.cfg.get("overlay_focus")):
            self.win.focus_set()
            self._focus_mode = True
        self._warp_cursor()
        self._input_guard.arm(self._game_hwnd, show_cursor=True)
        self._cursor_keeper()

    def _walk_widgets(self):
        stack = [self.win]
        while stack:
            w = stack.pop()
            yield w
            try:
                stack.extend(w.winfo_children())
            except tk.TclError:
                pass

    def _set_takefocus(self, enabled):
        """Focus hygiene: with overlay_focus off, strip takefocus from every
        widget so a click can never route into Tk's focus command (Tk focus
        -> TkpChangeFocus -> SetFocus activates the toplevel and steals the
        game's foreground). Originals are restored when focus mode is on."""
        if self._tf_saved is None:
            self._tf_saved = {}
            for w in self._walk_widgets():
                try:
                    self._tf_saved[w] = w.cget("takefocus")
                except tk.TclError:
                    pass
        for w, orig in self._tf_saved.items():
            try:
                w.configure(takefocus=orig if enabled else 0)
            except tk.TclError:
                pass

    def _game_covers_screen(self):
        """True when the game window spans ~the whole monitor it lives on.
        That is the only case where an escaped click could minimize the
        game / escape into an invisible desktop - so it is also the only
        case where the cursor jail (and the open-time cursor warp) earns
        its keep. Multi-monitor safe: compares against the game's OWN
        monitor work area, not the primary screen (winfo_screenwidth)."""
        gh = getattr(self, "_game_hwnd", None)
        if not gh:
            return False
        rect = wintypes.RECT()
        if not _u32.GetWindowRect(gh, ctypes.byref(rect)):
            return False
        wa = monitor_work_area_for_hwnd(gh)
        if wa:
            mw, mh = wa[2] - wa[0], wa[3] - wa[1]
        else:
            l, t, r, b = self._vroot_bounds()
            mw, mh = r - l, b - t
        gw = rect.right - rect.left
        ghh = rect.bottom - rect.top
        return gw >= mw * 0.9 and ghh >= mh * 0.9

    def _warp_cursor(self):
        """Park the OS cursor over the panel so the user SEES it at once
        (fullscreen games pin the invisible cursor to the screen center).
        Windowed games show a live cursor already - warping would just
        yank the pointer away from wherever the user left it."""
        if not self._jail_cursor:
            return
        try:
            self.win.update_idletasks()
            if self._modal:
                # Centered panel: land the pointer in the panel's middle
                # so the first click lands on a control.
                x = self.win.winfo_rootx() + self.win.winfo_width() // 2
                y = self.win.winfo_rooty() + self.win.winfo_height() // 2
            else:
                # Framed layout: land the cursor on the SIDEBAR band (not
                # the center, which is the see-through hole) so the first
                # real click operates a control instead of passing to the
                # game.
                x = self.win.winfo_rootx() + min(
                    SIDEBAR_W // 2, self.win.winfo_width() // 2)
                y = self.win.winfo_rooty() + min(
                    200, self.win.winfo_height() // 2)
            _u32.SetCursorPos(x, y)
        except (tk.TclError, ValueError, OSError):
            pass

    def _cursor_keeper(self):
        """Fullscreen-only jail: while a screen-covering game is up, the
        pointer must not escape the panel (an escaped click reaches the
        desktop/taskbar). ClipCursor/ReleaseCapture/ShowCursor
        housekeeping moved to OverlayInputGuard (overlay_input.py), which
        runs its own ~180 ms watchdog for BOTH modes - this 32 ms poll
        now only runs the jail, and is skipped mid-drag (_dragging /
        p30_panel.is_dragging: a drag that wanders off the edge is not a
        jailbreak)."""
        if self._inlayer:
            # In-layer has no visible window to jail over - the input
            # guard still runs (it only releases the game's confinement).
            return
        if not self.win.winfo_viewable():
            return
        held = False
        try:
            held = bool(_u32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        except (ValueError, OSError):
            pass
        if not held:
            # A ButtonRelease landing outside the panel never reaches our
            # bindtags - drop a stale flag so the keeper cannot wedge.
            self._dragging = False
        dragging = held or self._dragging
        try:
            if self.p30_panel and self.p30_panel.is_dragging():
                dragging = True
        except tk.TclError:
            pass
        if not dragging and not self._focus_mode and self._jail_cursor:
            self._keep_cursor_over_panel()
        self.win.after(CURSOR_KEEP_MS, self._cursor_keeper)

    def _keep_cursor_over_panel(self):
        try:
            point = wintypes.POINT()
            if not _u32.GetCursorPos(ctypes.byref(point)):
                return
            left = self.win.winfo_rootx()
            top = self.win.winfo_rooty()
            right = left + self.win.winfo_width()
            bottom = top + self.win.winfo_height()
            if not (left <= point.x < right and top <= point.y < bottom):
                _u32.SetCursorPos(left + min(24, self.win.winfo_width() // 2),
                                  top + min(40, self.win.winfo_height() // 2))
        except (tk.TclError, ValueError, OSError):
            pass

    def _return_focus(self):
        restore_focus = self._focus_mode
        self._focus_mode = False
        if restore_focus and self._game_hwnd:
            try:
                if _u32.GetForegroundWindow() != self._game_hwnd:
                    _u32.SetForegroundWindow(self._game_hwnd)
            except (ValueError, OSError):
                pass
        self._game_hwnd = None

    # ------------------------------------------------------------ show/hide
    def _is_shown(self):
        """Logical visibility. In in-layer mode the toplevel is ALWAYS
        viewable (mapped, invisible on-screen) so winfo_viewable() is not
        the truth - _ui_visible is."""
        if self._inlayer:
            return self._ui_visible
        return self.win.winfo_viewable()

    def toggle(self):
        if self._is_shown():
            self.hide()
        else:
            self.show()

    def _overlay_mode(self):
        """cfg overlay_mode: 'inlayer' (default - composited into the
        game frame by the Vulkan layer via shared memory) or 'external'
        (the classic real toplevel, kept as the fallback)."""
        m = str(self.ctl.cfg.get("overlay_mode") or "inlayer").lower()
        return "external" if m == "external" else "inlayer"

    def _inlayer_ready(self):
        """Arm the shm channel for the active game pid. False -> fall
        back to the external path; an open failure is logged ONCE per pid
        and that pid then stays external for the rest of the session.
        Every decision point is logged - the silent fallback is what made
        the field report undiagnosable."""
        try:
            pid = self.ctl.active_game_pid()
        except Exception as e:
            self._log("[overlay] active_game_pid raised %r" % e)
            pid = None
        if not pid:
            # No managed running game: nothing to composite into.
            # (game not in cfg games, launched externally, or exe-name
            # mismatch - the monitor snapshot only tracks cfg games)
            self._log("[overlay] in-layer: no active_game_pid -> "
                      "external (game unmanaged or not running)")
            return False
        if self._il is None:
            self._il = InLayerUI(self)
        il = self._il
        if il.is_armed() and il.pid != pid:
            self._log("[overlay] in-layer: active pid %s -> %s, "
                      "re-arming" % (il.pid, pid))
            il.disarm()           # game restarted / different game now
        if not il.is_armed():
            if pid == self._shm_failed_pid:
                self._log("[overlay] in-layer: pid %s latched external "
                          "(earlier shm/consumer failure)" % pid)
                return False
            try:
                armed = il.arm(pid)
            except Exception as e:
                armed = False
                self._log("[overlay] in-layer arm() raised %r for pid "
                          "%s - external fallback" % (e, pid))
            if not armed:
                self._shm_failed_pid = pid
                self._log("[overlay] in-layer shm unavailable for pid "
                            "%s - falling back to external mode" % pid)
                return False
        return True

    def show(self):
        mode = self._overlay_mode()
        try:
            pid = self.ctl.active_game_pid()
        except Exception:
            pid = None
        if mode == "inlayer" and self._inlayer_ready():
            self._log("[overlay] show: mode=inlayer pid=%s" % pid)
            self._show_inlayer()
            return
        self._log("[overlay] show: mode=%s pid=%s -> external" % (
            mode, pid))
        self._show_external()

    def _show_inlayer(self):
        """In-layer show: pixels go to the layer via the shm section,
        input comes back through the event ring. The toplevel stays
        MAPPED, on-screen but invisible (alpha=0 + click-through) - Tk
        keeps painting for PrintWindow, every binding keeps working,
        nothing ever lands on the desktop. All focus/cursor machinery is
        skipped on purpose. Framed layout: the window covers the game
        client, the center hole stays alpha-0."""
        freeze_enabled = bool(self.ctl.cfg.get("overlay_freeze"))
        il = self._il
        pid = il.pid
        # The game hwnd anchors CLIENT-coord placement (and the foreground
        # restore path is never taken in-layer, but _game_hwnd also feeds
        # _save_freeze's mode check helpers elsewhere).
        self._game_hwnd = hwnd_for_pid(pid) if pid else None
        if not self._game_hwnd:
            try:
                fg = _u32.GetForegroundWindow()
                if fg and fg != self._hwnd:
                    self._game_hwnd = fg
            except Exception:
                pass
        self._log("[overlay] in-layer game hwnd=%s pid=%s" % (
            "0x%x" % self._game_hwnd if self._game_hwnd else None, pid))
        self._jail_cursor = False     # no visible window to jail over;
                                      # the input guard still frees the
                                      # game's confinement below

        self.scale_var.set(scale_to_preset_label(self.ctl.cfg.get("scale")))
        # the combo re-seed above drops any mid-rebuild queued pick; the
        # apply WINDOW itself survives (a rebuild does not stop at hide)
        self._scale_queued = False
        self._scale_state_update()
        self.fg_var.set(bool(self.ctl.cfg.get("frame_gen")))
        sh = float(self.ctl.cfg.get("sharpness") or 0.8)
        self.sharpness_var.set(sh)
        self.sharpness_val.config(text="%.2f" % sh)
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        game_mode = mode in ("dx9", "dx11", "dx12", "vulkan")

        if freeze_enabled:
            gamelaunch.freeze_set(True)
            self._freeze_flags = True
            self.freeze_lbl.config(text=tr("ov_freeze_on"), fg=AMBER)
        else:
            self._freeze_flags = False
            self.freeze_lbl.config(text="")
        self._dragging = False

        # Map BEFORE arming: the window must be viewable for Tk to paint
        # it (and for the first synchronous publish to see pixels). The
        # park is ON-SCREEN but invisible - -alpha 0 keeps Tk's paint
        # flowing (off-screen windows get clipped to an empty visible
        # region) and WS_EX_TRANSPARENT lets clicks fall through to the
        # game, where the layer captures them for the event ring.
        hwnd = _toplevel_hwnd(self.win)
        self._hwnd = hwnd
        il._hwnd = hwnd
        if hwnd is not None:
            set_clickthrough(hwnd, True)
        self._inlayer = True          # before fit: in-layer clamps to cap
        self._ui_visible = True
        self.win.attributes("-alpha", 0.0)
        self.win.deiconify()
        self._fit_geometry(force=True)   # cover the game client rect
        covers = self._game_covers_screen()
        il.set_frame_flags(covers)
        self._combo_bindtags_off()
        il.armed()                # first frame + ui_visible + drain pump
        if self._modal:
            # Modal: the whole client is UI, so the keyboard belongs to
            # the overlay for the whole session - ESC closes the panel
            # and game keys never leak through. _focus_changed ORs this
            # into its focus-driven flag; hide()/disarm() clear the bit.
            self._kbd_session = True
            self._entry_kbd = True
            il.shm.set_kbd_capture(True)
        # Same confinement-release watchdog as external mode, minus the
        # ShowCursor/SetCursor half: the layer composites the sprite
        # cursor itself, but a game that pins the pointer (ClipCursor)
        # would otherwise starve the wndproc of the input it routes.
        # pin_only only when the game covers the screen: there the layer
        # keeps its own ClipCursor armed (covers the client, edge-tracked
        # - a blind release would kill it for the session); windowed sets
        # NOCLIP so every armed clip is foreign and released outright.
        self._input_guard.arm(self._game_hwnd, show_cursor=False,
                              pin_only=covers)
        self._start_fit()         # ~500 ms client-rect tracking
        self._start_esc()         # ESC closes the modal (belt: ring also
                                # delivers KeyPress Escape -> _esc_hide)
        self._log("[overlay] in-layer shown pid=%s hwnd=0x%x extent=%s"
                    % (pid or 0, hwnd or 0, self._extent))

        if self.ap_var.get() and game_mode and not gamelaunch.mode_paused(mode):
            self.ctl.submit(self.ctl.processing_toggle)
            self._log("[overlay] auto-paused (%s)" % mode)

        self.win.after(STATUS_MS, self._refresh)

    def _inlayer_lost(self):
        """The armed game pid is gone or the section died (the in-process
        layer died with it). Drop the session so the next toggle
        re-evaluates from scratch. Tk thread only - called from the
        publish pump's pid check / open check."""
        self._log("[overlay] in-layer session lost pid=%s" % (
            self._il.pid if self._il else None))
        if self._il:
            self._il.disarm()
        self._inlayer = False
        self._ui_visible = False
        self._combo_bindtags_restore()
        self.hide()         # external branch now: withdraw + unfreeze

    def _inlayer_dead(self):
        """Armed and visible, but no layer ever opened the section
        (LAYER_WAIT_S elapsed): the game has no consuming layer - the
        invisible window can never surface. Fall back to a REAL external
        window so the user actually gets the overlay. Latched per pid via
        _shm_failed_pid so every toggle does not re-probe a dead path."""
        pid = self._il.pid if self._il else None
        self._shm_failed_pid = pid
        self._log("[overlay] in-layer dead path for pid=%s - "
                  "switching to external window" % pid)
        if self._il:
            self._il.disarm()
        self._inlayer = False
        self._ui_visible = False
        self._combo_bindtags_restore()
        self._show_external()

    def _combo_bindtags_off(self):
        """In-layer only: drop the TCombobox class binding so <Button-1>
        cannot post the dropdown popdown (a separate real toplevel that
        would open at the parked position - invisible AND holding Tk's
        event grab, which would wedge the panel). Clicks cycle values
        instead via _combo_click. Applies to BOTH comboboxes (scale +
        Phase30 style)."""
        if self._combo_tags_saved is not None:
            return
        saved = {}
        for cb in (self.scale_cb,
                   getattr(self.p30_panel, "cb_style", None)):
            if cb is None:
                continue
            try:
                saved[cb] = cb.bindtags()
                cb.bindtags((cb, self.win, "all"))
                cb.bind("<ButtonPress-1>",
                        lambda e, c=cb: self._combo_click(c), add="+")
            except tk.TclError:
                pass
        self._combo_tags_saved = saved or None

    def _combo_bindtags_restore(self):
        if self._combo_tags_saved is not None:
            for cb, tags in self._combo_tags_saved.items():
                try:
                    cb.bindtags(tags)
                except tk.TclError:
                    pass
            self._combo_tags_saved = None

    def _combo_click(self, combo):
        """In-layer combobox click: cycle values (the real popdown cannot
        be composited into the game frame - see _combo_bindtags_off)."""
        if not self._inlayer:
            return None           # external mode keeps the real popdown
        try:
            values = [str(v) for v in (combo.cget("values") or [])]
            cur = combo.get()
            nxt = values[(values.index(cur) + 1) % len(values)] \
                if cur in values else values[0]
            combo.set(nxt)
            combo.event_generate("<<ComboboxSelected>>")
        except tk.TclError:
            pass
        return "break"

    def _scale_cycle(self, _e=None):
        """Compat shim - scale combo cycling is handled by _combo_click."""
        return self._combo_click(self.scale_cb)

    def _show_external(self):
        """Classic on-screen window (overlay_mode 'external' or the
        in-layer fallback) - now the FRAMED variant: the toplevel covers
        the game client rect, -transparentcolor punches the center hole
        (click-through pixels for free), and the band widgets sit on the
        opaque-ish perimeter."""
        if self._inlayer:
            # Left in-layer mode (config flip or dead game): drop the shm
            # session before mapping a real on-screen window.
            self._inlayer = False
            self._ui_visible = False
            self._combo_bindtags_restore()
            if self._il:
                self._il.disarm()
        focus_enabled = bool(self.ctl.cfg.get("overlay_focus"))
        freeze_enabled = bool(self.ctl.cfg.get("overlay_freeze"))
        try:
            fg_before = _u32.GetForegroundWindow() or 0
        except (ValueError, OSError):
            fg_before = 0

        # NON-ACTIVATION INVARIANT (1+2): resolve the top-level HWND and
        # (re)apply WS_EX_NOACTIVATE BEFORE anything that can map the window
        # (geometry/deiconify). The style was already set in __init__ while
        # withdrawn; this covers a Tk rebuild of the HWND and is idempotent.
        hwnd = _toplevel_hwnd(self.win)
        self._hwnd = hwnd        # cached for _reassert_topmost
        if hwnd is not None:
            _set_noactivate(hwnd, not focus_enabled)
            set_clickthrough(hwnd, False)   # in-layer's fall-through undo:
            # the external band is a real window - clicks must land on it
            if not focus_enabled:
                # WM_MOUSEACTIVATE -> MA_NOACTIVATE: WS_EX_NOACTIVATE
                # keeps the toplevel out of the foreground, but a click
                # on this host still demoted the exclusive-fullscreen
                # game (field report). Answering "no activate" ourselves
                # removes even that path while clicks still reach Tk for
                # widget hit-testing. Focus mode keeps the real answer.
                subclass_noactivate(hwnd)
        self.win.attributes("-alpha", self._alpha)   # undo in-layer's 0
        if self._modal:
            # Modal: the window must stay a SOLID dimmed surface - no
            # chroma key, no see-through hole.
            try:
                self.win.attributes("-transparentcolor", "")
            except tk.TclError:
                pass
        else:
            try:
                # The center hole: chroma-keyed pixels are fully
                # transparent AND click-through - the OS forwards them
                # to the game below. Band pixels keep -alpha.
                self.win.attributes("-transparentcolor", CHROMA)
            except tk.TclError:
                self._log("[overlay] -transparentcolor unsupported on "
                          "this Tk build - hole shows as solid chroma")

        # Remember the game window so hide() can hand the foreground back
        # (only used when focus mode was enabled for this session) - and so
        # the fit anchors the band on a WINDOWED game's client rect instead
        # of covering the whole monitor miles away from it.
        pid = self.ctl.active_game_pid()
        if pid:
            self._game_hwnd = hwnd_for_pid(pid)
        else:
            try:
                fg = _u32.GetForegroundWindow()
                if fg and fg != hwnd:
                    self._game_hwnd = fg
            except Exception:
                pass

        # Size/position the band to the game client rect (before map so
        # the first frame already covers it).
        self._fit_geometry(force=True)
        # Jail the cursor only when the game owns the whole screen;
        # windowed mode keeps the pointer free (and skips the warp).
        self._jail_cursor = self._game_covers_screen()
        self.scale_var.set(scale_to_preset_label(self.ctl.cfg.get("scale")))
        self._scale_queued = False     # combo re-seeded to applied value
        self._scale_state_update()
        self.fg_var.set(bool(self.ctl.cfg.get("frame_gen")))
        sh = float(self.ctl.cfg.get("sharpness") or 0.8)
        self.sharpness_var.set(sh)
        self.sharpness_val.config(text="%.2f" % sh)
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        game_mode = mode in ("dx9", "dx11", "dx12", "vulkan")

        # Frame freeze is opt-in (cfg overlay_freeze); the game keeps
        # running behind the panel either way. hide() unfreezes regardless.
        if freeze_enabled:
            gamelaunch.freeze_set(True)
            self._freeze_flags = True
            self.freeze_lbl.config(text=tr("ov_freeze_on"), fg=AMBER)
        else:
            self._freeze_flags = False
            self.freeze_lbl.config(text="")

        # Cursor freedom/visibility is owned by the input guard now: an
        # immediate pass frees the pointer at once, then a ~180 ms
        # watchdog re-releases while visible (games re-arm per frame).
        self._input_guard.arm(self._game_hwnd, show_cursor=True)
        self._dragging = False

        # Display the panel topmost WITHOUT taking the foreground. Tk's
        # deiconify honors WS_EX_NOACTIVATE on this host (probe: foreground
        # unchanged); _show_noactivate is the explicit win32 equivalent
        # (SW_SHOWNOACTIVATE + SWP_NOACTIVATE) so the mapped state never
        # depends on Tk internals. No lift(): SetWindowPos(HWND_TOPMOST,
        # SWP_NOACTIVATE) is the deterministic topmost re-assert.
        self.win.deiconify()
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        if not focus_enabled and hwnd is not None:
            _show_noactivate(hwnd)
        self._set_takefocus(focus_enabled)
        if focus_enabled and hwnd is not None:
            _force_foreground(hwnd)
            self.win.focus_force()
            self._focus_mode = True
        else:
            self._focus_mode = False

        def _log_shown():
            try:
                fg_after = _u32.GetForegroundWindow() or 0
            except (ValueError, OSError):
                fg_after = 0
            self._log("[overlay] shown freeze=%d focus=%d hwnd=0x%x "
                        "exstyle=0x%x fg_before=0x%x fg_after=0x%x" % (
                            freeze_enabled, focus_enabled, hwnd or 0,
                            _exstyle(hwnd) if hwnd else 0,
                            fg_before, fg_after))
        self.win.after(50, _log_shown)

        self._warp_cursor()
        self._cursor_keeper()
        self._start_fit()        # ~500 ms client-rect tracking
        self._start_esc()        # ESC closes (our toplevel is never
                               # focused, so no OS <Escape> reaches it)
        if self._modal:
            # Own cursor window: the game's SetCapture redirects
            # WM_SETCURSOR to itself (the invisible-cursor field
            # report), so a small click-through topmost toplevel tracks
            # GetCursorPos - immune to cursor-state races.
            if self._extcursor is None:
                self._extcursor = ExternalCursor(self.win)
            self._extcursor.show()

        if self.ap_var.get() and game_mode and not gamelaunch.mode_paused(mode):
            self.ctl.submit(self.ctl.processing_toggle)
            self._log("[overlay] auto-paused (%s)" % mode)

        self.win.after(STATUS_MS, self._refresh)

    def unfreeze(self):
        """Release any freeze; called on hide AND on manager close."""
        gamelaunch.freeze_set(False)
        if self._freeze_flags:
            self._freeze_flags = False
            self._log("[overlay] freeze released - game continues")

    def hide(self):
        self._cancel_fit()
        self._cancel_esc()
        self._input_guard.disarm()   # stop re-releasing the game's clip
        self._log("[overlay] hidden (inlayer=%d)" % self._inlayer)
        self.unfreeze()
        self.freeze_lbl.config(text="")
        self._return_focus()
        if self._extcursor is not None:
            self._extcursor.hide()
        self._kbd_session = False
        if self._entry_kbd and self._il and self._il.is_armed():
            self._il.shm.set_kbd_capture(False)
        self._entry_kbd = False
        if self._inlayer:
            # In-layer: the toplevel stays MAPPED at its invisible
            # on-screen park spot (Tk keeps painting for the next
            # publish); only the layer-side ui_visible flag drops -
            # compositing AND input capture stop together.
            self._ui_visible = False
            if self._il:
                self._il.hidden()
            return
        self.win.withdraw()


# Backwards compatibility for older imports/tests.
GainKnob = ControlOverlay
