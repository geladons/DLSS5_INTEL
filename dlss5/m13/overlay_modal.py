# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_modal - the Steam/Game-Bar style MODAL panel: a centered
# control card on a dimmed full-client backdrop (cfg overlay_style
# "modal", the default; "band" keeps overlay_widgets.build_panel).
#
# Layout: the toplevel still covers the whole game client (in-layer the
# shm hole is degenerate - the whole bitmap is UI, so ALL client input
# routes to the ring and the layer's cursor sprite stays visible
# everywhere). Everything outside the centered panel is published with a
# ~55% alpha scrim (DibCapture's opaque-rect split - straight alpha, no
# ABI change; the panel rect itself publishes fully opaque). External
# mode is the same tree under -alpha 0.95.
#
# Panel structure (min(880, W-40) x min(600, H-40), centered):
#   header  - "DLSS 5" + game exe name, state badge (ON/PAUSED/OFF),
#             applied preset name, close X
#   nav     - left column, 4 tabs: Presets | Image | Frame gen | System
#   content - the active tab page
#   footer  - status line + hotkey hints + Close
#
# build_modal_panel(ov) is the drop-in replacement for
# overlay_widgets.build_panel(ov): every widget/var lands on the overlay
# instance under the same attribute names so overlay.py needs no
# forked logic (scale_cb is None - modal has no combobox; band-only
# attrs like sidebar/strip are simply absent).
# ============================================================================
import ctypes
import tkinter as tk
from tkinter import ttk
from ctypes import wintypes

from .i18n import tr, SCALE_PRESETS, scale_to_preset_label
from .phase30 import Phase30Panel
from .overlay_hotkey import hotkey_label
from .overlay_widgets import (BG, PANEL, PANEL2, FG, MUTED, ACCENT,
                              AMBER, CHROMA)
from .overlay_win32 import (_u32, _toplevel_hwnd, _set_noactivate,
                            set_clickthrough, HWND_TOPMOST,
                            SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE)

DIMBG = "#0b0d12"          # toplevel bg: the scrim pixels under the panel
PANEL_MAX_W = 880
PANEL_MAX_H = 600
PANEL_MARGIN = 20          # per side -> client 800x600 yields 760x560
NAV_W = 170

TABS = ("presets", "image", "fg", "system")


class _WheelProxy:
    """ui.register_wheel_region target for the modal overlay: forwards
    wheel scrolls to the scroll canvas of the currently visible tab page
    (None while the active page has no scroller)."""

    def __init__(self):
        self.target = None

    def yview_scroll(self, n, units):
        t = self.target
        if t is not None:
            t.yview_scroll(n, units)

    def winfo_viewable(self):
        t = self.target
        try:
            return bool(t is not None and t.winfo_viewable())
        except tk.TclError:
            return False


def _scale_wheel(e, scale):
    """Wheel over a slider nudges it by one resolution step (Tk Scale has
    no native wheel binding). scale.set() re-fires its -command, so the
    debounced push follows on its own."""
    try:
        res = float(scale.cget("resolution") or 1)
        lo = float(scale.cget("from"))
        hi = float(scale.cget("to"))
        v = scale.get() + (res if e.delta > 0 else -res)
        scale.set(min(hi, max(lo, v)))
    except (tk.TclError, ValueError):
        pass
    return "break"


def _slider(parent, label, var, lo, hi, res, on_cmd, val_lbl_text="%.2f"):
    """Labeled horizontal slider with a numeric value readout; returns
    (scale, value_label)."""
    box = tk.Frame(parent, bg=BG)
    box.pack(fill="x", pady=(4, 2))
    hdr = tk.Frame(box, bg=BG)
    hdr.pack(fill="x")
    tk.Label(hdr, text=label, bg=BG, fg=FG,
             font=("Segoe UI", 10)).pack(side="left")
    val = tk.Label(hdr, text=val_lbl_text % float(var.get()),
                   bg=BG, fg=ACCENT, font=("Consolas", 10, "bold"))
    val.pack(side="right")
    sc = tk.Scale(box, from_=lo, to=hi, resolution=res,
                  orient="horizontal", variable=var, command=on_cmd,
                  bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0,
                  bd=0, activebackground=ACCENT, showvalue=False,
                  sliderrelief="flat", font=("Segoe UI", 8))
    sc.pack(fill="x")
    sc.bind("<MouseWheel>", lambda e, s=sc: _scale_wheel(e, s), add="+")
    return sc, val


def _page(ov, content, key, scroll=False):
    """One tab page: a Frame packed into `content` on selection. scroll=
    True wraps the inner content in a canvas+vscrollbar registered as the
    page's wheel target."""
    page = tk.Frame(content, bg=BG)
    if not scroll:
        inner = tk.Frame(page, bg=BG, padx=14, pady=12)
        inner.pack(fill="both", expand=True)
        return page, inner, None
    canvas = tk.Canvas(page, bg=BG, highlightthickness=0, borderwidth=0)
    vsb = ttk.Scrollbar(page, orient="vertical", command=canvas.yview)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side="right", fill="y")
    canvas.pack(side="left", fill="both", expand=True)
    inner = tk.Frame(canvas, bg=BG, padx=14, pady=12)
    win_id = canvas.create_window((0, 0), window=inner, anchor="nw")
    inner.bind("<Configure>", lambda _e, c=canvas: c.configure(
        scrollregion=c.bbox("all")))
    canvas.bind("<Configure>", lambda e, c=canvas, i=win_id:
                c.itemconfigure(i, width=e.width))
    return page, inner, canvas


def build_modal_panel(ov):
    """Construct the modal widget tree on a ControlOverlay instance."""
    w = ov.win
    w.configure(bg=DIMBG)

    ov._opaque = (0, 0, 0, 0)      # panel rect in bitmap coords (scrim)
    ov._tab = None
    ov._pages = {}
    ov._nav_btns = {}
    ov._page_canvas = {}
    ov.body_canvas = _WheelProxy()

    panel = ov.modal_panel = tk.Frame(
        w, bg=PANEL, highlightthickness=1, highlightbackground=PANEL2)
    # First paint before any fit; _place_panel() re-centers on every
    # geometry tick. Plain x/y only: a leftover relx/anchor would MERGE
    # with the absolute placement place() merges unmentioned options.
    panel.place(x=0, y=0, width=760, height=560)

    # ---------------------------------------------------------- header --
    hdr = tk.Frame(panel, bg=PANEL2, height=46)
    hdr.pack(fill="x")
    hdr.pack_propagate(False)
    tk.Label(hdr, text="◆ DLSS 5", bg=PANEL2, fg=ACCENT,
             font=("Segoe UI", 13, "bold")).pack(side="left",
                                                padx=(14, 6), pady=10)
    ov.game_lbl = tk.Label(hdr, text="", bg=PANEL2, fg=MUTED,
                           font=("Segoe UI", 10))
    ov.game_lbl.pack(side="left", pady=10)
    close = tk.Label(hdr, text="✕", bg=PANEL2, fg=MUTED,
                     font=("Segoe UI", 12, "bold"), padx=12,
                     cursor="hand2")
    close.pack(side="right")
    close.bind("<Button-1>", lambda _e: ov.hide())
    close.bind("<Enter>", lambda _e: close.config(fg="#f85149"))
    close.bind("<Leave>", lambda _e: close.config(fg=MUTED))
    # State badge - the ON/PAUSED/OFF pill _refresh() feeds.
    ov.state_lbl = tk.Label(hdr, text="", bg=PANEL2, fg=MUTED,
                            font=("Segoe UI", 10, "bold"))
    ov.state_lbl.pack(side="right", padx=(0, 2), pady=10)
    ov.preset_lbl = tk.Label(hdr, text="", bg=PANEL2, fg=MUTED,
                             font=("Segoe UI", 10))
    ov.preset_lbl.pack(side="right", padx=(0, 8), pady=10)

    # ----------------------------------------------------------- body --
    body = tk.Frame(panel, bg=BG)
    body.pack(fill="both", expand=True)

    nav = tk.Frame(body, bg=PANEL2, width=NAV_W)
    nav.pack(side="left", fill="y")
    nav.pack_propagate(False)
    for key in TABS:
        b = tk.Button(nav, text=tr("ov_tab_" + key), anchor="w",
                      font=("Segoe UI", 11), bg=PANEL2, fg=FG,
                      activebackground=PANEL, activeforeground=FG,
                      relief="flat", bd=0, padx=14, pady=10,
                      cursor="hand2",
                      command=lambda k=key: ov._tab_show(k))
        b.pack(fill="x")
        ov._nav_btns[key] = b

    content = tk.Frame(body, bg=BG)
    content.pack(side="left", fill="both", expand=True)

    # ------------------------------------------------------- presets ----
    page, inner, _cv = _page(ov, content, "presets")
    ov._pages["presets"] = page

    ov.scale_var = tk.StringVar(
        value=scale_to_preset_label(ov.ctl.cfg.get("scale")))
    ov.scale_cb = None              # modal has no combobox (see
                                    # _combo_bindtags_off)
    ov.preset_btns = {}
    seg = tk.Frame(inner, bg=BG)
    seg.pack(fill="x", pady=(0, 6))
    # Two-line segment cells (short name + percentage) in a uniform grid:
    # the full "Quality (67%)" labels overflow the content width at a
    # 760 px panel, so the pct moves to a second muted line. preset_btns
    # stays keyed by the FULL label - _preset_pick/_scale_selected and
    # tests address buttons by it.
    last = len(SCALE_PRESETS) - 1
    for i, (_val, key) in enumerate(SCALE_PRESETS):
        lbl = tr(key)
        seg.grid_columnconfigure(i, weight=1, uniform="seg")
        cell = tk.Frame(seg, bg=PANEL2, cursor="hand2",
                        highlightthickness=1, highlightbackground=PANEL)
        cell.grid(row=0, column=i, sticky="nsew",
                  padx=(0, 0 if i == last else 6))
        name = tr("scale_short_" + key.split("_", 1)[1])
        pct = lbl[lbl.rfind("(") + 1:-1] if "(" in lbl else ""
        nl = tk.Label(cell, text=name, bg=PANEL2, fg=FG,
                      font=("Segoe UI", 10, "bold"), cursor="hand2")
        nl.pack(pady=(8, 0))
        pl = tk.Label(cell, text=pct, bg=PANEL2, fg=MUTED,
                      font=("Segoe UI", 8), cursor="hand2")
        pl.pack(pady=(0, 7))
        for w_ in (cell, nl, pl):
            w_.bind("<Button-1>",
                    lambda _e, l=lbl: ov._preset_pick(l))
        cell._pct_lbl = pl     # dimmer second line in _preset_refresh
        ov.preset_btns[lbl] = cell
    # applied/applying/queued line (same contract as the band combo row)
    ov.scale_state = tk.Label(inner, text="", bg=BG, fg=MUTED,
                              font=("Segoe UI", 9), anchor="w",
                              justify="left")
    ov.scale_state.pack(fill="x", pady=(0, 10))

    ov.var = tk.DoubleVar(
        value=float(ov.ctl.cfg.get("gain")
                    if ov.ctl.cfg.get("gain") is not None else 1.0))
    ov._suppress_scale = None
    ov._scale_dragging = False

    def _gain_cmd(v):
        ov.gain_val.config(text="%.2f" % float(v))
        ov._debounced_push(v)

    _gs, ov.gain_val = _slider(inner, tr("ov_gain"), ov.var,
                               0.0, 2.0, 0.05, _gain_cmd)

    ov.blend_var = tk.DoubleVar(
        value=float(ov.ctl.cfg.get("blend") or 1.0))
    ov.blend_scale, ov.blend_val = _slider(
        inner, tr("ov_blend"), ov.blend_var, 0.0, 1.0, 0.05,
        ov._debounced_blend)

    ov.sharpness_var = tk.DoubleVar(
        value=float(ov.ctl.cfg.get("sharpness") or 0.8))
    ov.sharpness_scale, ov.sharpness_val = _slider(
        inner, tr("sharpness"), ov.sharpness_var, 0.0, 1.0, 0.05,
        ov._debounced_sharpness)

    # ---------------------------------------------------------- image ---
    page, inner, canvas = _page(ov, content, "image", scroll=True)
    ov._pages["image"] = page
    ov._page_canvas["image"] = canvas
    p30_wrap = tk.Frame(inner, bg=PANEL, highlightthickness=1,
                        highlightbackground=PANEL2)
    p30_wrap.pack(fill="x")
    ov.p30_panel = Phase30Panel(
        p30_wrap, initial_values=ov.ctl.cfg.get_phase30(),
        on_change=ov._on_phase30_change, on_reset=ov._on_phase30_reset,
        bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED,
        compact=False)
    ov.p30_panel.pack(fill="x", padx=8, pady=8)

    # ------------------------------------------------------ frame gen ---
    page, inner, _cv = _page(ov, content, "fg")
    ov._pages["fg"] = page
    ov.fg_var = tk.BooleanVar(value=bool(ov.ctl.cfg.get("frame_gen")))
    ov.fg_chk = tk.Checkbutton(
        inner, text=tr("framegen"), variable=ov.fg_var,
        command=ov._save_framegen, bg=BG, fg=FG, selectcolor=PANEL,
        activebackground=BG, activeforeground=FG,
        font=("Segoe UI", 10, "bold"), cursor="hand2")
    ov.fg_chk.pack(anchor="w", pady=(0, 6))
    ov.fg_stats_lbl = tk.Label(inner, text="", bg=BG, fg=MUTED,
                               font=("Consolas", 9), anchor="w",
                               justify="left")
    ov.fg_stats_lbl.pack(anchor="w")

    # --------------------------------------------------------- system ---
    page, inner, _cv = _page(ov, content, "system")
    ov._pages["system"] = page

    ov.toggle_btn = tk.Button(
        inner, text=tr("ov_pause"), font=("Segoe UI", 11, "bold"),
        bg=ACCENT, fg="#ffffff", activebackground="#3a70d6",
        activeforeground="#ffffff", relief="flat", bd=0, pady=8,
        cursor="hand2", command=ov._toggle_processing)
    ov.toggle_btn.pack(fill="x")
    ov.mode_lbl = tk.Label(inner, text="", bg=BG, fg=MUTED,
                           font=("Segoe UI", 9))
    ov.mode_lbl.pack(anchor="w", pady=(4, 10))

    ov.fr_var = tk.BooleanVar(value=bool(ov.ctl.cfg.get("overlay_freeze")))
    tk.Checkbutton(inner, text=tr("ov_freeze_opt"), variable=ov.fr_var,
                   command=ov._save_freeze, bg=BG, fg=FG,
                   selectcolor=PANEL, activebackground=BG,
                   activeforeground=FG, font=("Segoe UI", 10),
                   cursor="hand2").pack(anchor="w")
    ov.freeze_lbl = tk.Label(inner, text="", bg=BG, fg=AMBER,
                             font=("Segoe UI", 9), justify="left",
                             anchor="w", wraplength=460)
    ov.freeze_lbl.pack(fill="x", pady=(0, 6))

    ov.ap_var = tk.BooleanVar(
        value=bool(ov.ctl.cfg.get("overlay_autopause")))
    tk.Checkbutton(inner, text=tr("ov_autopause"), variable=ov.ap_var,
                   command=ov._save_autopause, bg=BG, fg=FG,
                   selectcolor=PANEL, activebackground=BG,
                   activeforeground=FG, font=("Segoe UI", 10),
                   cursor="hand2").pack(anchor="w", pady=(0, 10))

    tk.Label(inner, text=tr("ov_log_tail"), bg=BG, fg=FG,
             font=("Segoe UI", 10, "bold")).pack(anchor="w")
    ov.syslog_lbl = tk.Label(inner, text="", bg=PANEL, fg=MUTED,
                             font=("Consolas", 8), justify="left",
                             anchor="nw", padx=8, pady=6)
    ov.syslog_lbl.pack(fill="x", pady=(2, 10))

    tk.Label(inner, text=tr("ov_hotkeys"), bg=BG, fg=FG,
             font=("Segoe UI", 10, "bold")).pack(anchor="w")
    tk.Label(inner, text=tr("ov_footer_hint", hotkey_label(
        ov.ctl.cfg.get("overlay_hotkey"))), bg=BG, fg=MUTED,
        font=("Segoe UI", 9), anchor="w", justify="left").pack(fill="x")

    # ---------------------------------------------------------- footer --
    foot = tk.Frame(panel, bg=PANEL2)
    foot.pack(fill="x", side="bottom")
    # Auto-height: the shortened hint still wraps to two lines rather
    # than clip when the free width is tight (narrow panels).
    foot.bind("<Configure>", lambda e: hint_lbl.configure(
        wraplength=max(80, e.width - 260)))
    ov.status = tk.Label(foot, text="", bg=PANEL2, fg=MUTED,
                         font=("Consolas", 9), anchor="w")
    ov.status.pack(side="left", padx=(12, 8), pady=8)
    hint_lbl = tk.Label(foot, text=tr("ov_footer_hint_short",
        hotkey_label(ov.ctl.cfg.get("overlay_hotkey"))), bg=PANEL2,
        fg="#565b66", font=("Segoe UI", 9), anchor="w", justify="left",
        wraplength=520)
    hint_lbl.pack(side="left", padx=8, pady=8)
    ttk.Button(foot, text=tr("ov_close"),
               command=ov.hide).pack(side="right", padx=(4, 12), pady=6)

    ov._tab_show("presets")


class ExternalCursor:
    """The external-modal overlay's OWN cursor: a 32x32 overrideredirect
    topmost toplevel with the same arrow sprite the layer draws, chroma-
    keyed + WS_EX_TRANSPARENT so clicks pass through to the panel below.
    The OS cursor's SetCursor/ShowCursor state stays at the mercy of the
    game (SetCapture redirects WM_SETCURSOR - a raw-input title leaves a
    NULL cursor over our window); this window is immune: it just tracks
    GetCursorPos at ~100 Hz while shown."""

    TICK_MS = 10
    HOTSPOT = (4, 2)                 # arrow tip inside the 32x32 sprite

    def __init__(self, master):
        self.win = tk.Toplevel(master)
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        try:
            self.win.attributes("-transparentcolor", CHROMA)
        except tk.TclError:
            pass
        c = tk.Canvas(self.win, width=32, height=32, bg=CHROMA,
                      highlightthickness=0, bd=0, takefocus=0)
        c.pack()
        c.create_polygon(4, 2, 4, 22, 9, 17, 12, 26, 15, 24, 12, 16,
                         19, 16, fill="#ffffff", outline="#000000")
        self.win.withdraw()
        self._id = None
        self._hwnd = None
        self._z_ticks = 0
        self._apply_styles()

    def _apply_styles(self):
        """(Re)resolve the HWND and apply NOACTIVATE + click-through.
        Tk recreates the top-level HWND across withdraw/deiconify, so the
        hwnd must be sampled AFTER the map, not cached from __init__."""
        self._hwnd = _toplevel_hwnd(self.win)
        if self._hwnd:
            _set_noactivate(self._hwnd, True)
            set_clickthrough(self._hwnd, True)

    def show(self):
        try:
            self.win.deiconify()
        except tk.TclError:
            return
        # Tk re-writes GWL_EXSTYLE while mapping (same race fix_popdown
        # documents for combobox popdowns): apply now, on <Map>, and once
        # ~60 ms later so the last write is ours.
        self._apply_styles()
        try:
            self.win.bind("<Map>", lambda _e: self._apply_styles())
            self.win.after(60, self._apply_styles)
        except tk.TclError:
            pass
        self._tick()

    def _tick(self):
        self._id = None
        try:
            pt = wintypes.POINT()
            if _u32.GetCursorPos(ctypes.byref(pt)):
                self.win.geometry("+%d+%d" % (pt.x - self.HOTSPOT[0],
                                              pt.y - self.HOTSPOT[1]))
        except (tk.TclError, ValueError, OSError):
            pass
        # The main overlay re-asserts HWND_TOPMOST ~1/s; a topmost window
        # loses to the most recent re-assert, which would park the sprite
        # UNDER the dim backdrop. Re-raise on a quarter-second cadence.
        self._z_ticks += 1
        if self._z_ticks >= 25:
            self._z_ticks = 0
            try:
                if self._hwnd:
                    _u32.SetWindowPos(self._hwnd, HWND_TOPMOST, 0, 0,
                                      0, 0, SWP_NOMOVE | SWP_NOSIZE
                                      | SWP_NOACTIVATE)
            except (ValueError, OSError):
                pass
        try:
            self._id = self.win.after(self.TICK_MS, self._tick)
        except tk.TclError:
            pass

    def hide(self):
        if self._id is not None:
            try:
                self.win.after_cancel(self._id)
            except Exception:
                pass
            self._id = None
        try:
            self.win.withdraw()
        except tk.TclError:
            pass
