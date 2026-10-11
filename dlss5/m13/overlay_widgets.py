# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_widgets - dark theme palette + framed control-band
# construction. ControlOverlay._build delegates here (build_panel(self));
# every widget and Tk variable lands on the overlay instance, callbacks
# bind to its methods - identical to an in-place _build, just split for
# file size.
#
# FRAMED LAYOUT (the band): the toplevel covers the GAME CLIENT rect and
# its own bg is CHROMA, the transparentcolor key. Two packed frames form
# the perimeter band - a left sidebar carrying every control and a slim
# bottom status strip - while the remaining center rect stays uncovered:
#   external mode: -transparentcolor CHROMA punches a real see-through,
#                  click-through hole (the OS forwards those pixels).
#   in-layer mode: the same rect is published as the shm bitmap hole
#                  (alpha 0 + hole rect in the header - see overlay_shm).
# Pack order matters: the strip goes bottom/full-width FIRST, then the
# sidebar fills the rest of the left edge - so the hole is always the
# single rect (sidebar_w, 0, W, H - strip_h).
# ============================================================================
import tkinter as tk
from tkinter import ttk

from .i18n import (tr, scale_preset_labels, scale_to_preset_label)
from .phase30 import Phase30Panel
from .overlay_win32 import fix_popdown
from .overlay_hotkey import hotkey_label

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

CHROMA = "#ff00ff"   # hole color key: never used by any band widget
SIDEBAR_W = 240      # control column width (spec: ~200-260 px)
STRIP_H = 30         # bottom status strip height


def build_panel(ov):
    """Construct the framed widget tree of a ControlOverlay instance (the
    former ControlOverlay._build body; ov is the overlay instance)."""
    w = ov.win
    w.configure(bg=CHROMA)

    # ---- bottom status strip (packed first => full width) -------------
    ov.strip = tk.Frame(w, bg=PANEL, height=STRIP_H)
    ov.strip.pack(side="bottom", fill="x")
    ov.strip.pack_propagate(False)
    ov.freeze_lbl = tk.Label(
        ov.strip, text="", bg=PANEL, fg=AMBER, font=("Segoe UI", 8),
        justify="left", anchor="w")
    ov.freeze_lbl.pack(side="left", padx=(8, 4))
    ov.status = tk.Label(ov.strip, text="", bg=PANEL, fg=MUTED,
                         font=("Consolas", 8), justify="left", anchor="w")
    ov.status.pack(side="left", padx=4, fill="x", expand=True)
    tk.Label(ov.strip, text=tr("ov_strip_hint", hotkey_label(
        ov.ctl.cfg.get("overlay_hotkey"))), bg=PANEL, fg="#565b66",
        font=("Segoe UI", 8)).pack(side="right", padx=(4, 8))

    # ---- left sidebar (fills the remainder of the left edge) ----------
    ov.sidebar = tk.Frame(w, bg=BG, width=SIDEBAR_W)
    ov.sidebar.pack(side="left", fill="y")
    ov.sidebar.pack_propagate(False)

    # title strip (label + close; the framed band is NOT dragged - the
    # geometry poll keeps it snapped to the game client rect)
    title_strip = tk.Frame(ov.sidebar, bg=PANEL, height=34)
    title_strip.pack(fill="x")
    title_strip.pack_propagate(False)
    ov.title_lbl = tk.Label(title_strip, text="◆ DLSS 5", bg=PANEL,
                            fg=ACCENT, font=("Segoe UI", 10, "bold"))
    ov.title_lbl.pack(side="left", padx=10)
    close = tk.Label(title_strip, text="✕", bg=PANEL, fg=MUTED,
                     font=("Segoe UI", 10, "bold"), padx=10, cursor="hand2")
    close.pack(side="right")
    # Processing-state pill (ON/PAUSED/OFF): also the in-game feedback
    # for the consumer's own CTRL+ALT+X/Q hotkeys - refreshed in _refresh.
    ov.state_lbl = tk.Label(title_strip, text="", bg=PANEL, fg=MUTED,
                            font=("Segoe UI", 8, "bold"))
    ov.state_lbl.pack(side="right", padx=(0, 2))
    close.bind("<Button-1>", lambda _e: ov.hide())
    close.bind("<Enter>", lambda _e: close.config(fg=RED))
    close.bind("<Leave>", lambda _e: close.config(fg=MUTED))

    body_shell = tk.Frame(ov.sidebar, bg=BG)
    body_shell.pack(fill="both", expand=True)
    ov.body_canvas = tk.Canvas(
        body_shell, bg=BG, highlightthickness=0, borderwidth=0)
    body_scroll = ttk.Scrollbar(
        body_shell, orient="vertical", command=ov.body_canvas.yview)
    ov.body_canvas.configure(yscrollcommand=body_scroll.set)
    body_scroll.pack(side="right", fill="y")
    ov.body_canvas.pack(side="left", fill="both", expand=True)
    body = tk.Frame(ov.body_canvas, bg=BG, padx=8, pady=10)
    ov._body_window = ov.body_canvas.create_window(
        (0, 0), window=body, anchor="nw")
    body.bind("<Configure>", lambda _e: ov.body_canvas.configure(
        scrollregion=ov.body_canvas.bbox("all")))
    ov.body_canvas.bind(
        "<Configure>", lambda e: ov.body_canvas.itemconfigure(
            ov._body_window, width=e.width))

    # processing toggle (the big one)
    ov.toggle_btn = tk.Button(
        body, text=tr("ov_pause"), font=("Segoe UI", 10, "bold"),
        bg=ACCENT, fg="#ffffff", activebackground="#3a70d6",
        activeforeground="#ffffff", relief="flat", bd=0, pady=7,
        cursor="hand2", command=ov._toggle_processing)
    ov.toggle_btn.pack(fill="x")

    ov.mode_lbl = tk.Label(body, text="", bg=BG, fg=MUTED,
                           font=("Segoe UI", 8))
    ov.mode_lbl.pack(anchor="w", pady=(3, 8))

    # Scale & FrameGen row
    top_row = tk.Frame(body, bg=BG)
    top_row.pack(fill="x", pady=(2, 4))
    tk.Label(top_row, text=tr("scale"), bg=BG, fg=FG,
             font=("Segoe UI", 8, "bold")).pack(side="left")
    ov.scale_var = tk.StringVar(
        value=scale_to_preset_label(ov.ctl.cfg.get("scale")))
    # width 18 so the FULL preset name stays readable (a scale change
    # rebuilds the engine ~6 s - the user must see what is applied)
    ov.scale_cb = ttk.Combobox(
        top_row, textvariable=ov.scale_var, state="readonly",
        values=scale_preset_labels(), width=18)
    ov.scale_cb.pack(side="left", padx=(6, 0))
    ov.scale_cb.bind("<<ComboboxSelected>>", ov._scale_selected)
    # The combobox popdown is a fresh Tk toplevel that does NOT
    # inherit our WS_EX_NOACTIVATE - showing it plain would be an
    # activation vector against exclusive-fullscreen games.
    ov.scale_cb.bind("<Button-1>", fix_popdown(ov.scale_cb),
                     add="+")

    # Preset feedback line: "applied preset: X" at rest; "applying: X
    # (~6 s rebuild)" while the consumer-side engine re-init runs, plus
    # "queued: Y" when a mid-rebuild click is deferred (see overlay.py
    # _scale_apply_start). Always shows the full name even when the
    # combobox itself would clip it.
    ov.scale_state = tk.Label(body, text="", bg=BG, fg=MUTED,
                              font=("Segoe UI", 7), anchor="w",
                              justify="left")
    ov.scale_state.pack(fill="x", pady=(0, 4))

    ov.fg_var = tk.BooleanVar(value=bool(ov.ctl.cfg.get("frame_gen")))
    ov.fg_chk = tk.Checkbutton(
        body, text=tr("framegen"), variable=ov.fg_var,
        command=ov._save_framegen, bg=BG, fg=FG, selectcolor=PANEL,
        activebackground=BG, activeforeground=FG,
        font=("Segoe UI", 8, "bold"), cursor="hand2")
    ov.fg_chk.pack(fill="x", pady=(0, 4))

    # Blend (stacked - the sidebar is too narrow for side-by-side)
    b_box = tk.Frame(body, bg=BG)
    b_box.pack(fill="x", pady=(2, 4))
    b_hdr = tk.Frame(b_box, bg=BG)
    b_hdr.pack(fill="x")
    tk.Label(b_hdr, text=tr("ov_blend"), bg=BG, fg=FG,
             font=("Segoe UI", 8, "bold")).pack(side="left")
    ov.blend_val = tk.Label(b_hdr, text="%.2f" % float(ov.ctl.cfg.get("blend") or 1.0),
                            bg=BG, fg=ACCENT, font=("Consolas", 8, "bold"))
    ov.blend_val.pack(side="right")
    ov.blend_var = tk.DoubleVar(value=float(ov.ctl.cfg.get("blend") or 1.0))
    ov.blend_scale = tk.Scale(
        b_box, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
        variable=ov.blend_var, command=ov._debounced_blend,
        bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
        activebackground=ACCENT, showvalue=False, sliderrelief="flat")
    ov.blend_scale.pack(fill="x")

    # Sharpness
    s_box = tk.Frame(body, bg=BG)
    s_box.pack(fill="x", pady=(2, 4))
    s_hdr = tk.Frame(s_box, bg=BG)
    s_hdr.pack(fill="x")
    tk.Label(s_hdr, text=tr("sharpness"), bg=BG, fg=FG,
             font=("Segoe UI", 8, "bold")).pack(side="left")
    ov.sharpness_val = tk.Label(
        s_hdr, text="%.2f" % float(ov.ctl.cfg.get("sharpness") or 0.8),
        bg=BG, fg=ACCENT, font=("Consolas", 8, "bold"))
    ov.sharpness_val.pack(side="right")
    ov.sharpness_var = tk.DoubleVar(
        value=float(ov.ctl.cfg.get("sharpness") or 0.8))
    ov.sharpness_scale = tk.Scale(
        s_box, from_=0.0, to=1.0, resolution=0.05, orient="horizontal",
        variable=ov.sharpness_var, command=ov._debounced_sharpness,
        bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
        activebackground=ACCENT, showvalue=False, sliderrelief="flat")
    ov.sharpness_scale.pack(fill="x")

    # Phase 30 Vendor Panel
    p30_wrap = tk.Frame(body, bg=PANEL, highlightthickness=1,
                        highlightbackground=PANEL2)
    p30_wrap.pack(fill="x", pady=(4, 6))
    ov.p30_panel = Phase30Panel(
        p30_wrap, initial_values=ov.ctl.cfg.get_phase30(),
        on_change=ov._on_phase30_change,
        on_reset=ov._on_phase30_reset,
        bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED,
        compact=True)
    ov.p30_panel.pack(fill="x", padx=6, pady=6)

    # Legacy variables for compatibility. ov.var is the GAIN knob's var
    # (_debounced_push -> ctl.set_gain, synced from daemon_gain): it must
    # seed from "gain" (residual multiplier), never "intensity".
    ov.var = tk.DoubleVar(
        value=float(ov.ctl.cfg.get("gain") if ov.ctl.cfg.get("gain") is not None else 1.0))
    ov._suppress_scale = None
    ov._scale_dragging = False

    # Options: freeze and autopause
    opts_row = tk.Frame(body, bg=BG)
    opts_row.pack(fill="x", pady=(2, 4))
    ov.fr_var = tk.BooleanVar(
        value=bool(ov.ctl.cfg.get("overlay_freeze")))
    tk.Checkbutton(opts_row, text=tr("ov_freeze_opt"),
                   variable=ov.fr_var, command=ov._save_freeze,
                   bg=BG, fg=MUTED, selectcolor=PANEL,
                   activebackground=BG, activeforeground=FG,
                   font=("Segoe UI", 8), cursor="hand2").pack(anchor="w")

    ov.ap_var = tk.BooleanVar(
        value=bool(ov.ctl.cfg.get("overlay_autopause")))
    tk.Checkbutton(opts_row, text=tr("ov_autopause"), variable=ov.ap_var,
                   command=ov._save_autopause, bg=BG, fg=MUTED,
                   selectcolor=PANEL, activebackground=BG,
                   activeforeground=FG, font=("Segoe UI", 8),
                   cursor="hand2").pack(anchor="w")
