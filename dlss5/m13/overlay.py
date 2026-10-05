# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay - in-game control overlay (hotkey, default CTRL+ALT+G).
# Frameless, always-on-top, semi-transparent panel centered over the screen.
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
#     from the widget tree. All foreground/cursor machinery is
#     external-only: in-layer never moves the OS cursor and never shows a
#     visible window on the desktop.
#
# Threading: the overlay never does I/O itself. It renders ctl.snapshot()
# (monitor thread) and routes every action through ctl.submit() (worker).
# The in-layer shm pumps are plain after() timers on the Tk thread -
# PrintWindow is a synchronous cross-thread WM_PRINT send, so it cannot
# live on a worker thread anyway (see InLayerUI).
# ============================================================================
import ctypes
import time
import tkinter as tk
from ctypes import wintypes

from .processes import hwnd_for_pid
from . import gamelaunch
from .i18n import (tr, scale_preset_labels, scale_to_preset_label,
                   preset_label_to_scale)
from .overlay_hotkey import (VK, POLL_MS, parse_hotkey,  # noqa: F401
                             hotkey_label, HotkeyPoller)
from .overlay_inlayer import InLayerUI
from .overlay_widgets import (build_panel, BG, PANEL, PANEL2, FG, MUTED,
                              ACCENT, GREEN, RED, AMBER)
from .overlay_win32 import (_u32, GWL_EXSTYLE, WS_EX_TOPMOST,  # noqa: F401
                            WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
                            HWND_TOPMOST, SWP_NOMOVE, SWP_NOSIZE,
                            SWP_NOACTIVATE,
                            _set_noactivate, _free_cursor, _input_jiggle,
                            _force_foreground, _toplevel_hwnd, _exstyle,
                            _show_noactivate,
                            set_clickthrough,
                            monitor_work_area_for_hwnd)

VK_LBUTTON = 0x01
DEBOUNCE_MS = 250
STATUS_MS = 500
CURSOR_KEEP_MS = 32
CURSOR_FREE_MS = 500   # low cadence: full ReleaseCapture/SetCursor pass
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
        self._alpha = 0.88
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
        self._drag = None
        self._freeze_flags = False   # primary freeze: layer/proxy flags
        self._pending_push = None    # (value, time) of an in-flight push
        self._after_id_blend = None  # debounce timer for the blend knob
        self._after_id_sharpness = None  # debounce timer for sharpness
        self._game_hwnd = None       # foreground restore target
        self._focus_mode = False     # we took the foreground this session
        self._jail_cursor = True     # recompute per show(): fullscreen only
        self._tf_saved = None        # original takefocus per widget
        self._dragging = False       # a mouse button is held over the panel
        self._last_cursor_free = 0.0  # last full _free_cursor() timestamp
        self._last_topmost = 0.0     # last HWND_TOPMOST re-assert timestamp
        self._hwnd = None            # cached top-level HWND (per show())
        self._inlayer = False        # this session composites via the layer
        self._ui_visible = False     # logical panel visibility (in-layer)
        self._il = None              # InLayerUI bridge, built on first arm
        self._shm_failed_pid = None  # shm open failed for this pid ->
                                     # external for the rest of that session
        self._combo_tags_saved = None  # scale_cb bindtags while in-layer
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
        # Widget-tree construction lives in overlay_widgets.build_panel -
        # it assigns every widget/var straight onto this instance.
        build_panel(self)

    # -------------------------------------------------------------- drag ---
    def _button_press(self, _e):
        """Any button held over the panel - _cursor_keeper must not run
        ReleaseCapture/SetCursor mid-gesture (kills Tk slider drags)."""
        self._dragging = True

    def _button_release(self, _e):
        self._dragging = False
        self._drag = None

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

    def _drag_start(self, e):
        if self._inlayer:
            # Synthetic ring events carry GAME-CLIENT coords in
            # x_root/y_root (InLayerUI._inject); the parked window's own
            # position is the meaningless park spot. Dragging moves the
            # dst rect the layer composites at - not the hidden window.
            self._drag = (e.x_root - self._il.dst[0],
                          e.y_root - self._il.dst[1])
            self._dragging = True
            return
        self._drag = (e.x_root - self.win.winfo_x(),
                      e.y_root - self.win.winfo_y())
        self._dragging = True

    def _drag_move(self, e):
        if self._inlayer:
            if self._drag:
                self._il.set_dst(e.x_root - self._drag[0],
                                 e.y_root - self._drag[1])
            return
        if self._drag:
            # Clamp to the virtual desktop so the panel can never be
            # parked offscreen (negative/secondary-monitor coords are real).
            x = e.x_root - self._drag[0]
            y = e.y_root - self._drag[1]
            l, t, r, b = self._vroot_bounds()
            x = min(max(l, x), max(l, r - self.win.winfo_width()))
            y = min(max(t, y), max(t, b - self.win.winfo_height()))
            self.win.geometry("+%d+%d" % (x, y))

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
        self.on_log("[overlay] Phase30 %s -> %s" % (key, val))

    def _on_phase30_reset(self):
        self.ctl.submit(self.ctl.reset_phase30)
        self.on_log("[overlay] Phase30 reset to defaults")

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
        lbl = self.scale_var.get()
        s = preset_label_to_scale(lbl)
        self.ctl.submit(self.ctl.set_scale, s)
        self.on_log("[overlay] scale set to %.2f (%s)" % (s, lbl))

    def _save_framegen(self):
        fg = bool(self.fg_var.get())
        self.ctl.submit(self.ctl.set_framegen, fg)
        self.on_log("[overlay] frame generation %s" % (
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
        self.scale_var.set(scale_to_preset_label(s))

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
                self.on_log("[overlay] frame frozen (flags); knob turns reprocess the original")
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
        paused = self.ctl.processing_paused()
        if paused is True:
            self.toggle_btn.config(text=tr("ov_resume"), bg=GREEN,
                                   activebackground="#2ea043")
        elif paused is False:
            self.toggle_btn.config(text=tr("ov_pause"), bg=ACCENT,
                                   activebackground="#3a70d6")
        else:
            self.toggle_btn.config(text=tr("ov_pause"), bg=PANEL,
                                   activebackground=PANEL2)
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
        gh = self._game_hwnd
        self.win.after(100, lambda: _free_cursor(gh))
        self.win.after(350, lambda: _free_cursor(gh))
        self.win.after(700, lambda: _free_cursor(gh))
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
            x = self.win.winfo_rootx() + self.win.winfo_width() // 2
            y = self.win.winfo_rooty() + 40
            _u32.SetCursorPos(x, y)
        except (tk.TclError, ValueError, OSError):
            pass

    def _cursor_keeper(self):
        """While the panel is open, keep the cursor free: games re-apply
        ClipCursor/ShowCursor(false) on every frame, so the one-shot at
        show() is not enough. The full ReleaseCapture/SetCursor/ClipCursor
        pass is NOT run at this 32 ms poll rate - it killed Tk slider
        drags (ReleaseCapture) and fought widget cursors (SetCursor).
        It runs once at show() and then at CURSOR_FREE_MS cadence, and the
        whole keeper skips while a mouse button is held over the panel
        (_dragging: slider drags, title-strip moves, popdown picks). The
        hard jail (teleport back into the panel) only runs for a
        fullscreen game and is also skipped mid-drag."""
        if self._inlayer:
            # In-layer never touches the OS cursor: the layer draws the
            # sprite and clips the pointer itself. The invisible mapped
            # toplevel is always viewable, so the check below alone
            # cannot stop the chain - kill it explicitly on mode entry.
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
        if not dragging:
            now = time.time()
            if now - self._last_cursor_free >= CURSOR_FREE_MS / 1000.0:
                _free_cursor(self._game_hwnd)
                self._last_cursor_free = now
            if not self._focus_mode and self._jail_cursor:
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
        and that pid then stays external for the rest of the session."""
        try:
            pid = self.ctl.active_game_pid()
        except Exception:
            pid = None
        if not pid:
            return False          # no game -> nothing to composite into
        if self._il is None:
            self._il = InLayerUI(self)
        il = self._il
        if il.is_armed() and il.pid != pid:
            il.disarm()           # game restarted / different game now
        if not il.is_armed():
            if pid == self._shm_failed_pid:
                return False
            if not il.arm(pid):
                self._shm_failed_pid = pid
                self.on_log("[overlay] in-layer shm unavailable for pid "
                            "%s - falling back to external mode" % pid)
                return False
        return True

    def show(self):
        if self._overlay_mode() == "inlayer" and self._inlayer_ready():
            self._show_inlayer()
            return
        self._show_external()

    def _show_inlayer(self):
        """In-layer show: pixels go to the layer via the shm section,
        input comes back through the event ring. The toplevel stays
        MAPPED, on-screen but invisible (alpha=0 + click-through) - Tk
        keeps painting for PrintWindow, every binding keeps working,
        nothing ever lands on the desktop. All focus/cursor machinery is
        skipped on purpose."""
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
        self._jail_cursor = False     # never touch the OS cursor in-layer

        self.scale_var.set(scale_to_preset_label(self.ctl.cfg.get("scale")))
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

        # Same centre-over-the-game convention as _center(), expressed in
        # the game's CLIENT coords for the layer.
        il.place(self._game_hwnd)

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
        px, py = il.park_pos()
        self.win.geometry("%dx%d+%d+%d" % (self.WIDTH, self.HEIGHT, px, py))
        self.win.attributes("-alpha", 0.0)
        self.win.deiconify()
        self._inlayer = True
        self._ui_visible = True
        self._combo_bindtags_off()
        il.armed()                # first frame + ui_visible + drain pump
        self.on_log("[overlay] in-layer shown pid=%s hwnd=0x%x dst=%d,%d"
                    % (pid or 0, hwnd or 0, il.dst[0], il.dst[1]))

        if self.ap_var.get() and game_mode and not gamelaunch.mode_paused(mode):
            self.ctl.submit(self.ctl.processing_toggle)
            self.on_log("[overlay] auto-paused (%s)" % mode)

        self.win.after(STATUS_MS, self._refresh)

    def _inlayer_lost(self):
        """The armed game pid is gone or the section died (the in-process
        layer died with it). Drop the session so the next toggle
        re-evaluates from scratch. Tk thread only - called from the
        publish pump's pid check / open check."""
        if self._il:
            self._il.disarm()
        self._inlayer = False
        self._ui_visible = False
        self._combo_bindtags_restore()
        self.hide()         # external branch now: withdraw + unfreeze

    def _combo_bindtags_off(self):
        """In-layer only: drop the TCombobox class binding so <Button-1>
        cannot post the dropdown popdown (a separate real toplevel that
        would open at the parked position - invisible AND holding Tk's
        event grab, which would wedge the panel). Click cycles presets
        instead via _scale_cycle."""
        if self._combo_tags_saved is not None:
            return
        try:
            self._combo_tags_saved = self.scale_cb.bindtags()
            self.scale_cb.bindtags((self.scale_cb, self.win, "all"))
            self.scale_cb.bind("<ButtonPress-1>", self._scale_cycle,
                               add="+")
        except tk.TclError:
            self._combo_tags_saved = None

    def _combo_bindtags_restore(self):
        if self._combo_tags_saved is not None:
            try:
                self.scale_cb.bindtags(self._combo_tags_saved)
            except tk.TclError:
                pass
            self._combo_tags_saved = None

    def _scale_cycle(self, _e=None):
        """In-layer scale combo click: cycle presets (the real popdown
        cannot be composited into the game frame - see _combo_bindtags_off)."""
        if not self._inlayer:
            return None           # external mode keeps the real popdown
        labels = scale_preset_labels()
        cur = self.scale_var.get()
        nxt = labels[(labels.index(cur) + 1) % len(labels)] \
            if cur in labels else labels[0]
        self.scale_var.set(nxt)
        self._scale_selected()
        return "break"

    def _show_external(self):
        """Classic on-screen panel (overlay_mode 'external' or the
        in-layer fallback). The original show() body."""
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
            # the external panel is a real window - clicks must land on it
        self.win.attributes("-alpha", self._alpha)   # undo in-layer's 0

        # Remember the game window so hide() can hand the foreground back
        # (only used when focus mode was enabled for this session) - and so
        # _center() can anchor the panel over a WINDOWED game instead of
        # landing on the middle of the monitor miles away from it.
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

        self._center()
        # Jail the cursor only when the game owns the whole screen;
        # windowed mode keeps the pointer free (and skips the warp).
        self._jail_cursor = self._game_covers_screen()
        self.scale_var.set(scale_to_preset_label(self.ctl.cfg.get("scale")))
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

        # Free the cursor, release capture on both threads, set arrow cursor
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        _free_cursor(self._game_hwnd)
        self._last_cursor_free = time.time()
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
            self.on_log("[overlay] shown freeze=%d focus=%d hwnd=0x%x "
                        "exstyle=0x%x fg_before=0x%x fg_after=0x%x" % (
                            freeze_enabled, focus_enabled, hwnd or 0,
                            _exstyle(hwnd) if hwnd else 0,
                            fg_before, fg_after))
        self.win.after(50, _log_shown)

        self._warp_cursor()
        gh = self._game_hwnd
        self.win.after(100, lambda: _free_cursor(gh))
        self.win.after(350, lambda: _free_cursor(gh))
        self.win.after(700, lambda: _free_cursor(gh))
        self._cursor_keeper()

        if self.ap_var.get() and game_mode and not gamelaunch.mode_paused(mode):
            self.ctl.submit(self.ctl.processing_toggle)
            self.on_log("[overlay] auto-paused (%s)" % mode)

        self.win.after(STATUS_MS, self._refresh)

    def unfreeze(self):
        """Release any freeze; called on hide AND on manager close."""
        gamelaunch.freeze_set(False)
        if self._freeze_flags:
            self._freeze_flags = False
            self.on_log("[overlay] freeze released - game continues")

    def _center(self):
        """Center on the game window when one is known (windowed games put
        the play area away from screen-center); fall back to the virtual
        desktop. Multi-monitor: clamps to the GAME monitor's work area -
        a secondary screen is not at (0,0) and primary-screen clamps would
        fling the panel off the monitor the game actually lives on."""
        rect = wintypes.RECT()
        gh = getattr(self, "_game_hwnd", None)
        if gh and _u32.GetWindowRect(gh, ctypes.byref(rect)):
            gw = rect.right - rect.left
            ghh = rect.bottom - rect.top
            if gw > 0 and ghh > 0:
                x = rect.left + max(0, (gw - self.WIDTH) // 2)
                y = rect.top + max(0, (ghh - self.HEIGHT) // 2)
                wa = monitor_work_area_for_hwnd(gh) or self._vroot_bounds()
                x = min(max(wa[0], x), max(wa[0], wa[2] - self.WIDTH))
                y = min(max(wa[1], y), max(wa[1], wa[3] - self.HEIGHT))
                self.win.geometry(
                    "%dx%d+%d+%d" % (self.WIDTH, self.HEIGHT, x, y))
                return
        l, t, r, b = self._vroot_bounds()
        x = l + max(0, (r - l - self.WIDTH) // 2)
        y = t + max(0, (b - t - self.HEIGHT) // 3)
        self.win.geometry("%dx%d+%d+%d" % (self.WIDTH, self.HEIGHT, x, y))

    def hide(self):
        self.unfreeze()
        self.freeze_lbl.config(text="")
        self._return_focus()
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
