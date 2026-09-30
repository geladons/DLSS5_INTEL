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
# PHOTO MODE (opt-in, cfg overlay_freeze): open the overlay -> the frame
# FREEZES (the layer/proxy holds
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
from ctypes import wintypes
from tkinter import ttk

from .processes import suspend_pid, resume_pid, hwnd_for_pid
from . import gamelaunch
from .i18n import (tr, scale_preset_labels, scale_to_preset_label,
                   preset_label_to_scale)
from .phase30 import Phase30Panel
from .overlay_win32 import (_u32, GWL_EXSTYLE, WS_EX_TOPMOST,  # noqa: F401
                            WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
                            _set_noactivate, _free_cursor, _input_jiggle,
                            _force_foreground)

VK = {"control": 0x11, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
POLL_MS = 120
DEBOUNCE_MS = 250
STATUS_MS = 500
CURSOR_KEEP_MS = 32

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


class ControlOverlay:
    """Frameless always-on-top control panel floating over the game."""

    WIDTH, HEIGHT = 470, 710

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
        self._tf_saved = None        # original takefocus per widget
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

        body_shell = tk.Frame(w, bg=BG)
        body_shell.pack(fill="both", expand=True)
        self.body_canvas = tk.Canvas(
            body_shell, bg=BG, highlightthickness=0, borderwidth=0)
        body_scroll = ttk.Scrollbar(
            body_shell, orient="vertical", command=self.body_canvas.yview)
        self.body_canvas.configure(yscrollcommand=body_scroll.set)
        body_scroll.pack(side="right", fill="y")
        self.body_canvas.pack(side="left", fill="both", expand=True)
        body = tk.Frame(self.body_canvas, bg=BG, padx=14, pady=10)
        self._body_window = self.body_canvas.create_window(
            (0, 0), window=body, anchor="nw")
        body.bind("<Configure>", lambda _e: self.body_canvas.configure(
            scrollregion=self.body_canvas.bbox("all")))
        self.body_canvas.bind(
            "<Configure>", lambda e: self.body_canvas.itemconfigure(
                self._body_window, width=e.width))

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

        # Scale & FrameGen row
        top_row = tk.Frame(body, bg=BG)
        top_row.pack(fill="x", pady=(2, 4))
        tk.Label(top_row, text=tr("scale"), bg=BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        self.scale_var = tk.StringVar(
            value=scale_to_preset_label(self.ctl.cfg.get("scale")))
        self.scale_cb = ttk.Combobox(
            top_row, textvariable=self.scale_var, state="readonly",
            values=scale_preset_labels(), width=18)
        self.scale_cb.pack(side="left", padx=(6, 12))
        self.scale_cb.bind("<<ComboboxSelected>>", self._scale_selected)

        def _fix_popdown(_e):
            try:
                popdown = self.win.tk.eval(
                    "ttk::combobox::PopdownWindow %s" % self.scale_cb)
                self.win.tk.eval("wm attributes %s -topmost 1" % popdown)
            except Exception:
                pass
        self.scale_cb.bind("<Button-1>", _fix_popdown, add="+")

        self.fg_var = tk.BooleanVar(value=bool(self.ctl.cfg.get("frame_gen")))
        self.fg_chk = tk.Checkbutton(
            top_row, text=tr("framegen"), variable=self.fg_var,
            command=self._save_framegen, bg=BG, fg=FG, selectcolor=PANEL,
            activebackground=BG, activeforeground=FG,
            font=("Segoe UI", 8, "bold"), cursor="hand2")
        self.fg_chk.pack(side="right")

        # Blend & Sharpness row
        bs_row = tk.Frame(body, bg=BG)
        bs_row.pack(fill="x", pady=(2, 4))

        # Blend
        b_box = tk.Frame(bs_row, bg=BG)
        b_box.pack(side="left", fill="x", expand=True, padx=(0, 6))
        b_hdr = tk.Frame(b_box, bg=BG)
        b_hdr.pack(fill="x")
        tk.Label(b_hdr, text=tr("ov_blend"), bg=BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        self.blend_val = tk.Label(b_hdr, text="%.2f" % float(self.ctl.cfg.get("blend") or 1.0),
                                  bg=BG, fg=ACCENT, font=("Consolas", 8, "bold"))
        self.blend_val.pack(side="right")
        self.blend_var = tk.DoubleVar(value=float(self.ctl.cfg.get("blend") or 1.0))
        self.blend_scale = tk.Scale(
            b_box, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
            variable=self.blend_var, command=self._debounced_blend,
            bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.blend_scale.pack(fill="x")

        # Sharpness
        s_box = tk.Frame(bs_row, bg=BG)
        s_box.pack(side="right", fill="x", expand=True, padx=(6, 0))
        s_hdr = tk.Frame(s_box, bg=BG)
        s_hdr.pack(fill="x")
        tk.Label(s_hdr, text=tr("sharpness"), bg=BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        self.sharpness_val = tk.Label(
            s_hdr, text="%.2f" % float(self.ctl.cfg.get("sharpness") or 0.8),
            bg=BG, fg=ACCENT, font=("Consolas", 8, "bold"))
        self.sharpness_val.pack(side="right")
        self.sharpness_var = tk.DoubleVar(
            value=float(self.ctl.cfg.get("sharpness") or 0.8))
        self.sharpness_scale = tk.Scale(
            s_box, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
            variable=self.sharpness_var, command=self._debounced_sharpness,
            bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.sharpness_scale.pack(fill="x")

        # Phase 30 Vendor Panel
        p30_wrap = tk.Frame(body, bg=PANEL, highlightthickness=1,
                            highlightbackground=PANEL2)
        p30_wrap.pack(fill="x", pady=(4, 6))
        self.p30_panel = Phase30Panel(
            p30_wrap, initial_values=self.ctl.cfg.get_phase30(),
            on_change=self._on_phase30_change,
            on_reset=self._on_phase30_reset,
            bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED,
            compact=True)
        self.p30_panel.pack(fill="x", padx=8, pady=6)

        # Legacy variables for compatibility
        self.var = tk.DoubleVar(
            value=float(self.ctl.cfg.get("intensity") if self.ctl.cfg.get("intensity") is not None else 1.0))
        self._suppress_scale = None
        self._scale_dragging = False

        # Options: freeze and autopause
        opts_row = tk.Frame(body, bg=BG)
        opts_row.pack(fill="x", pady=(2, 4))
        self.fr_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        tk.Checkbutton(opts_row, text=tr("ov_freeze_opt"),
                       variable=self.fr_var, command=self._save_freeze,
                       bg=BG, fg=MUTED, selectcolor=PANEL,
                       activebackground=BG, activeforeground=FG,
                       font=("Segoe UI", 8), cursor="hand2").pack(side="left")

        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        tk.Checkbutton(opts_row, text=tr("ov_autopause"), variable=self.ap_var,
                       command=self._save_autopause, bg=BG, fg=MUTED,
                       selectcolor=PANEL, activebackground=BG,
                       activeforeground=FG, font=("Segoe UI", 8),
                       cursor="hand2").pack(side="right")

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
        if hasattr(self, "p30_panel") and self.p30_panel:
            self.p30_panel.set_values({"intensity": g})
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
        if hasattr(self, "p30_panel") and self.p30_panel:
            self.p30_panel.set_values({"intensity": 1.0})

    def _on_phase30_change(self, key, val):
        if key == "intensity":
            self.var.set(val)
        self.ctl.submit(self.ctl.set_phase30, {key: val})
        self.on_log("[overlay] Phase30 %s -> %s" % (key, val))

    def _on_phase30_reset(self):
        self.ctl.submit(self.ctl.reset_phase30)
        self.var.set(1.0)
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

        self.win.after(STATUS_MS, self._refresh)


    # ------------------------------------------------------- focus / cursor
    def _take_focus(self):
        _u32.ReleaseCapture()
        _u32.ClipCursor(None)
        _free_cursor()
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        self.win.lift()
        if bool(self.ctl.cfg.get("overlay_focus")):
            self.win.focus_set()
            self._focus_mode = True
        self._warp_cursor()
        self.win.after(100, _free_cursor)
        self.win.after(350, _free_cursor)
        self.win.after(700, _free_cursor)
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
        if not self._focus_mode:
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

        focus_enabled = bool(self.ctl.cfg.get("overlay_focus"))
        freeze_enabled = bool(self.ctl.cfg.get("overlay_freeze"))
        try:
            fg_before = _u32.GetForegroundWindow() or 0
        except (ValueError, OSError):
            fg_before = 0

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
        _free_cursor()

        try:
            hwnd = int(self.win.wm_frame(), 16)
            _set_noactivate(hwnd, not focus_enabled)
        except (tk.TclError, ValueError):
            hwnd = None

        # Display the panel topmost WITHOUT taking the foreground
        # (deiconify/lift do not activate on this host - see
        # _probe_noactivate.py). Only steal focus when explicitly enabled.
        self.win.deiconify()
        self.win.configure(cursor="arrow")
        self.win.attributes("-topmost", True)
        self.win.lift()
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
            self.on_log("[overlay] shown freeze=%d focus=%d "
                        "fg_before=0x%x fg_after=0x%x" % (
                            freeze_enabled, focus_enabled,
                            fg_before, fg_after))
        self.win.after(50, _log_shown)

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
