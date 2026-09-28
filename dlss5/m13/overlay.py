# ============================================================================
# m13.overlay - in-game control overlay (hotkey, default CTRL+ALT+G).
# Frameless, always-on-top, semi-transparent panel centered over the screen.
#
# MOUSE & NOACTIVATE:
# The overlay window ALWAYS maintains WS_EX_TOPMOST | WS_EX_NOACTIVATE |
# WS_EX_TOOLWINDOW styles. It never steals Windows foreground focus from the
# game, so borderless and exclusive-fullscreen games NEVER minimize when the
# overlay is opened. Clicks, drags, and selections on the overlay work cleanly
# without activating the window, while _free_cursor() and _cursor_keeper()
# keep the mouse cursor visible and free of game clipping rectangles.
#
# PHOTO MODE: open the overlay -> the frame FREEZES (the layer/proxy holds
# the raw frame and re-blits its processed result, the game keeps running
# underneath). Turn the gain/blend/scale/sharpness knob -> knob push +
# reprocess flag bump -> the SAME original frame is reprocessed and the frozen
# picture updates with live results.
# Close -> the freeze flag drops, the game continues with the new settings.
#
# Threading: the overlay never does I/O itself. It renders ctl.snapshot()
# (monitor thread) and routes every action through ctl.submit() (worker).
# ============================================================================
import ctypes
import time
import tkinter as tk
from tkinter import ttk

from .processes import suspend_pid, resume_pid, hwnd_for_pid
from . import gamelaunch
from .i18n import (tr, scale_preset_labels, scale_to_preset_label,
                   preset_label_to_scale)

VK = {"control": 0x11, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
POLL_MS = 120
DEBOUNCE_MS = 250
STATUS_MS = 500

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080

# dark theme (shared palette with ui.py)
BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

_u32 = ctypes.windll.user32
SW_RESTORE = 9
IDC_ARROW = 32512


def parse_hotkey(spec):
    """'control+alt+g' -> (mods_mask, vk). Defaults to ctrl+alt+g."""
    mods = 0
    key = "g"
    parts = [p.strip().lower() for p in (spec or "").split("+") if p.strip()]
    if parts:
        key = parts[-1]
        for p in parts[:-1]:
            if p in VK:
                mods |= {0x11: 0x0002, 0x12: 0x0001, 0x10: 0x0004}[VK[p]]
    vk = ord(key[0].upper()) if key else ord("G")
    return mods, vk


def hotkey_label(spec):
    return (spec or "control+alt+g").upper().replace("CONTROL", "CTRL")


class HotkeyPoller:
    """Edge-triggered global combo polling."""

    def __init__(self, spec, on_fire, poll_ms=POLL_MS):
        self.mods, self.vk = parse_hotkey(spec)
        self.on_fire = on_fire
        self.poll_ms = poll_ms
        self._armed = True
        self._tk = None

    def _down(self, vk):
        return bool(_u32.GetAsyncKeyState(vk) & 0x8000)

    def poll(self):
        combo = self._down(self.vk)
        if combo and self.mods:
            combo = all(self._down(vk) for bit, vk in
                        ((0x0002, VK["control"]), (0x0001, VK["alt"]),
                         (0x0004, VK["shift"])) if self.mods & bit)
        if combo and self._armed:
            self._armed = False
            self.on_fire()
        elif not combo:
            self._armed = True
        if self._tk is not None:
            self._tk.after(self.poll_ms, self.poll)

    def attach(self, tk_widget):
        self._tk = tk_widget
        tk_widget.after(self.poll_ms, self.poll)


def _set_noactivate(hwnd, on=True):
    try:
        style = _u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        style |= WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
        _u32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)
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


class ControlOverlay:
    """Frameless always-on-top control panel floating over the game."""

    WIDTH, HEIGHT = 460, 490

    def __init__(self, master, ctl, on_log):
        self.ctl = ctl
        self.on_log = on_log
        self.win = tk.Toplevel(master)
        self.win.overrideredirect(True)          # frameless: no OS window look
        self.win.attributes("-topmost", True)
        self.win.attributes("-alpha", 0.95)
        self.win.configure(bg=BG, highlightthickness=1,
                           highlightbackground=ACCENT, cursor="arrow")
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.hide)
        self.win.withdraw()
        self._after_id = None
        self._drag = None
        self._frozen_pid = None      # fallback freeze: suspended game pid
        self._freeze_flags = False   # primary freeze: layer/proxy flags
        self._pending_push = None    # (value, time) of an in-flight push
        self._after_id_blend = None  # debounce timer for the blend knob
        self._after_id_sharpness = None  # debounce timer for sharpness
        self._game_hwnd = None       # foreground restore target
        self._focus_mode = False     # we took the foreground this session
        self._build()
        self.poller = HotkeyPoller(ctl.cfg.get("overlay_hotkey"), self.toggle)
        self.poller.attach(master)

    # ------------------------------------------------------------- build ---
    def _build(self):
        w = self.win
        # title strip (drag handle)
        strip = tk.Frame(w, bg=PANEL, height=34)
        strip.pack(fill="x")
        strip.pack_propagate(False)
        self.title_lbl = tk.Label(strip, text="◆ DLSS 5", bg=PANEL, fg=ACCENT,
                                  font=("Segoe UI", 10, "bold"))
        self.title_lbl.pack(side="left", padx=10)
        close = tk.Label(strip, text="✕", bg=PANEL, fg=MUTED,
                         font=("Segoe UI", 10, "bold"), padx=10, cursor="hand2")
        close.pack(side="right")
        close.bind("<Button-1>", lambda _e: self.hide())
        close.bind("<Enter>", lambda _e: close.config(fg=RED))
        close.bind("<Leave>", lambda _e: close.config(fg=MUTED))
        for widget in (strip, self.title_lbl):
            widget.bind("<ButtonPress-1>", self._drag_start)
            widget.bind("<B1-Motion>", self._drag_move)

        body = tk.Frame(w, bg=BG, padx=14, pady=10)
        body.pack(fill="both", expand=True)

        self.freeze_lbl = tk.Label(
            body, text="", bg=BG, fg=AMBER, font=("Segoe UI", 8),
            justify="left", anchor="w", wraplength=self.WIDTH - 32)
        self.freeze_lbl.pack(fill="x", pady=(0, 6))

        # processing toggle (the big one)
        self.toggle_btn = tk.Button(
            body, text=tr("ov_pause"), font=("Segoe UI", 10, "bold"),
            bg=ACCENT, fg="#ffffff", activebackground="#3a70d6",
            activeforeground="#ffffff", relief="flat", bd=0, pady=7,
            cursor="hand2", command=self._toggle_processing)
        self.toggle_btn.pack(fill="x")

        self.mode_lbl = tk.Label(body, text="", bg=BG, fg=MUTED,
                                 font=("Segoe UI", 8))
        self.mode_lbl.pack(anchor="w", pady=(3, 8))

        # gain
        grow = tk.Frame(body, bg=BG)
        grow.pack(fill="x")
        tk.Label(grow, text=tr("ov_gain"), bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        tk.Button(grow, text=tr("ov_reset"), font=("Segoe UI", 7),
                  bg=PANEL, fg=MUTED, activebackground=PANEL2,
                  activeforeground=FG, relief="flat", bd=0, padx=6,
                  cursor="hand2", command=self._reset_gain).pack(
            side="left", padx=(8, 0))
        self.gain_val = tk.Label(grow, text="1.00", bg=BG, fg=ACCENT,
                                 font=("Consolas", 10, "bold"), width=5)
        self.gain_val.pack(side="right")
        self.var = tk.DoubleVar(value=self.ctl.cfg.get("gain"))
        self._suppress_scale = None   # programmatic set echo suppression:
        self._scale_dragging = False  # Tk fires the command DEFERRED
        self.scale = tk.Scale(
            body, from_=0.0, to=2.0, resolution=0.05, orient="horizontal",
            variable=self.var, command=self._debounced_push,
            bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.scale.pack(fill="x", pady=(0, 4))
        self.scale.bind("<ButtonPress-1>",
                        lambda _e: setattr(self, "_scale_dragging", True))
        self.scale.bind("<ButtonRelease-1>",
                        lambda _e: setattr(self, "_scale_dragging", False))

        # blend (effect mix: 0 = original frame, 1 = full network output)
        brow = tk.Frame(body, bg=BG)
        brow.pack(fill="x")
        tk.Label(brow, text=tr("ov_blend"), bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        tk.Button(brow, text=tr("ov_blend_reset"), font=("Segoe UI", 7),
                  bg=PANEL, fg=MUTED, activebackground=PANEL2,
                  activeforeground=FG, relief="flat", bd=0, padx=6,
                  cursor="hand2", command=self._reset_blend).pack(
            side="left", padx=(8, 0))
        self.blend_val = tk.Label(brow, text="1.00", bg=BG, fg=ACCENT,
                                  font=("Consolas", 10, "bold"), width=5)
        self.blend_val.pack(side="right")
        self.blend_var = tk.DoubleVar(value=float(
            self.ctl.cfg.get("blend") or 1.0))
        self.blend_scale = tk.Scale(
            body, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
            variable=self.blend_var, command=self._debounced_blend,
            bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.blend_scale.pack(fill="x", pady=(0, 4))

        # scale preset
        srow = tk.Frame(body, bg=BG)
        srow.pack(fill="x", pady=(4, 4))
        tk.Label(srow, text=tr("scale"), bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.scale_var = tk.StringVar(
            value=scale_to_preset_label(self.ctl.cfg.get("scale")))
        self.scale_cb = ttk.Combobox(
            srow, textvariable=self.scale_var, state="readonly",
            values=scale_preset_labels(), width=24)
        self.scale_cb.pack(side="right")
        self.scale_cb.bind("<<ComboboxSelected>>", self._scale_selected)

        def _fix_popdown(_e):
            try:
                popdown = self.win.tk.eval(
                    "ttk::combobox::PopdownWindow %s" % self.scale_cb)
                self.win.tk.eval("wm attributes %s -topmost 1" % popdown)
            except Exception:
                pass
        self.scale_cb.bind("<Button-1>", _fix_popdown, add="+")

        # frame generation toggle
        fgrow = tk.Frame(body, bg=BG)
        fgrow.pack(fill="x", pady=(2, 4))
        self.fg_var = tk.BooleanVar(value=bool(self.ctl.cfg.get("frame_gen")))
        self.fg_chk = tk.Checkbutton(
            fgrow, text=tr("framegen"), variable=self.fg_var,
            command=self._save_framegen, bg=BG, fg=FG, selectcolor=PANEL,
            activebackground=BG, activeforeground=FG,
            font=("Segoe UI", 9, "bold"), cursor="hand2")
        self.fg_chk.pack(side="left")
        tk.Label(fgrow, text=tr("framegen_hint"), bg=BG, fg=MUTED,
                 font=("Segoe UI", 7)).pack(side="right")

        # sharpness
        shrow = tk.Frame(body, bg=BG)
        shrow.pack(fill="x")
        tk.Label(shrow, text=tr("sharpness"), bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        tk.Button(shrow, text="Reset 0.8", font=("Segoe UI", 7),
                  bg=PANEL, fg=MUTED, activebackground=PANEL2,
                  activeforeground=FG, relief="flat", bd=0, padx=6,
                  cursor="hand2", command=self._reset_sharpness).pack(
            side="left", padx=(8, 0))
        self.sharpness_val = tk.Label(
            shrow, text="%.2f" % float(self.ctl.cfg.get("sharpness") or 0.8),
            bg=BG, fg=ACCENT, font=("Consolas", 10, "bold"), width=5)
        self.sharpness_val.pack(side="right")
        self.sharpness_var = tk.DoubleVar(
            value=float(self.ctl.cfg.get("sharpness") or 0.8))
        self.sharpness_scale = tk.Scale(
            body, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
            variable=self.sharpness_var, command=self._debounced_sharpness,
            bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.sharpness_scale.pack(fill="x", pady=(0, 4))

        prow = tk.Frame(body, bg=BG)
        prow.pack(fill="x", pady=(0, 4))
        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        tk.Checkbutton(prow, text=tr("ov_autopause"), variable=self.ap_var,
                       command=self._save_autopause, bg=BG, fg=MUTED,
                       selectcolor=PANEL, activebackground=BG,
                       activeforeground=FG, font=("Segoe UI", 8),
                       cursor="hand2").pack(side="right")

        frow = tk.Frame(body, bg=BG)
        frow.pack(fill="x", pady=(0, 6))
        self.fr_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        tk.Checkbutton(frow, text=tr("ov_freeze_opt"),
                       variable=self.fr_var, command=self._save_freeze,
                       bg=BG, fg=MUTED, selectcolor=PANEL,
                       activebackground=BG, activeforeground=FG,
                       font=("Segoe UI", 8), cursor="hand2").pack(side="left")

        self.status = tk.Label(body, text="", bg=BG, fg=MUTED,
                               font=("Consolas", 8), justify="left")
        self.status.pack(anchor="w")

        tk.Label(body, text=tr("ov_hide_hint", hotkey_label(
            self.ctl.cfg.get("overlay_hotkey"))), bg=BG, fg="#565b66",
            font=("Segoe UI", 8)).pack(side="bottom", pady=(8, 0))

    # -------------------------------------------------------------- drag ---
    def _drag_start(self, e):
        self._drag = (e.x_root - self.win.winfo_x(),
                      e.y_root - self.win.winfo_y())

    def _drag_move(self, e):
        if self._drag:
            self.win.geometry("+%d+%d" % (e.x_root - self._drag[0],
                                          e.y_root - self._drag[1]))

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
        self.gain_val.config(text="%.2f" % g)
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
        if not self.win.winfo_viewable():
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
        if snap.get("daemon_pid") is not None:
            g, f = snap.get("daemon_gain"), snap.get("daemon_frames")
            txt = tr("ov_daemon_up", snap["daemon_pid"])
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
                    self.gain_val.config(text="%.2f" % g)
            if snap.get("daemon_err"):
                txt += tr("daemon_busy")
            self.status.config(text=txt, fg=MUTED)
        elif snap.get("screen_pid") is not None:
            self.status.config(text=tr("ov_screen_up", snap["screen_pid"]),
                               fg=MUTED)
        else:
            self.status.config(text=tr("ov_daemon_down"), fg=RED)
        self.win.after(STATUS_MS, self._refresh)

    # ------------------------------------------------------- focus / cursor
    def _take_focus(self):
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        _free_cursor()
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        self.win.lift()
        self.win.focus_set()
        self._warp_cursor()
        self._focus_mode = True
        self.win.after(100, _free_cursor)
        self.win.after(350, _free_cursor)
        self.win.after(700, _free_cursor)
        self._cursor_keeper()

    def _warp_cursor(self):
        """Park the OS cursor over the panel so the user SEES it at once
        (many games pin the invisible cursor to the screen center)."""
        try:
            self.win.update_idletasks()
            x = self.win.winfo_rootx() + self.win.winfo_width() // 2
            y = self.win.winfo_rooty() + 40
            _u32.SetCursorPos(x, y)
        except (tk.TclError, ValueError, OSError):
            pass

    def _cursor_keeper(self):
        """While the panel is open, keep the cursor free: FPS games re-apply
        ClipCursor/ShowCursor(false) on every frame, one-shot is not enough."""
        if not self.win.winfo_viewable():
            return
        _free_cursor()
        self.win.after(150, self._cursor_keeper)

    def _return_focus(self):
        self._focus_mode = False
        if self._game_hwnd:
            try:
                if _u32.GetForegroundWindow() != self._game_hwnd:
                    _u32.SetForegroundWindow(self._game_hwnd)
            except (ValueError, OSError):
                pass
            self._game_hwnd = None

    # ------------------------------------------------------------ show/hide
    def toggle(self):
        if self.win.winfo_viewable():
            self.hide()
        else:
            self.show()

    def show(self):
        self._center()
        self.scale_var.set(scale_to_preset_label(self.ctl.cfg.get("scale")))
        self.fg_var.set(bool(self.ctl.cfg.get("frame_gen")))
        sh = float(self.ctl.cfg.get("sharpness") or 0.8)
        self.sharpness_var.set(sh)
        self.sharpness_val.config(text="%.2f" % sh)
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        game_mode = mode in ("dx9", "dx11", "dx12", "vulkan")

        # Remember the game window so hide() can hand the foreground back
        pid = self.ctl.active_game_pid()
        if pid:
            self._game_hwnd = hwnd_for_pid(pid)
        else:
            try:
                fg = _u32.GetForegroundWindow()
                ov_hwnd = int(self.win.wm_frame(), 16)
                if fg and fg != ov_hwnd:
                    self._game_hwnd = fg
            except Exception:
                pass

        # Always freeze the game on open as requested (presentation layer stands still)
        gamelaunch.freeze_set(True)
        self._freeze_flags = True
        self.freeze_lbl.config(text=tr("ov_freeze_on"), fg=AMBER)
        self.on_log("[overlay] frame frozen; mouse focus given to overlay")

        # Free the cursor, release capture on both threads, set arrow cursor
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        _free_cursor()

        # Display and focus the overlay without minimizing fullscreen games.
        # overrideredirect(True) + WS_EX_TOPMOST + focus_set() lets Tkinter
        # receive mouse clicks, drags, and key events, keeping the cursor visible.
        self.win.deiconify()
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        self.win.lift()
        self.win.focus_set()
        self._focus_mode = True

        self._warp_cursor()
        self.win.after(100, _free_cursor)
        self.win.after(350, _free_cursor)
        self.win.after(700, _free_cursor)
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
        if self._frozen_pid is not None:
            ok, msg = resume_pid(self._frozen_pid)
            self.on_log("[overlay] %s" % msg)
            self._frozen_pid = None

    def _center(self):
        sw = self.win.winfo_screenwidth()
        sh = self.win.winfo_screenheight()
        x = max(0, (sw - self.WIDTH) // 2)
        y = max(0, (sh - self.HEIGHT) // 3)
        self.win.geometry("%dx%d+%d+%d" % (self.WIDTH, self.HEIGHT, x, y))

    def hide(self):
        self.unfreeze()
        self.freeze_lbl.config(text="")
        self._return_focus()
        self.win.withdraw()


# Backwards compatibility for older imports/tests.
GainKnob = ControlOverlay
