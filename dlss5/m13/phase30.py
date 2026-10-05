# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.phase30 - NVIDIA Phase30 vendor tuning controls.
#
# Provides the 7 vendor parameters for compose-side blending, local tone,
# structure filtering, skin preservation, style index, and chroma control:
#   1. intensity: 0.0 - 2.0 (default 1.0 - public compose baseline)
#   2. local_tone: 0.0 - 2.0 (default 1.0)
#   3. local_structure: 0.0 - 2.0 (default 1.0)
#   4. skin_structure: -1.0 - 2.0 (default -1.0; disabled in public baseline)
#   5. auto_mask: bool (default False)
#   6. style_index: 0, 1, 2 (default 0: Default, 1: Natural, 2: Cinematic)
#   7. color_strength: 0.0 - 2.0 (default 1.0)
#
# The knob file also carries the engine knobs gain/blend/scale/sharpness/
# frame_gen: a RUNNING game cannot see env changes, so the in-process
# consumers (m11-layer, m12-dxgi) poll the file as the live channel while
# env stays the launch-time baseline.
#
# Handles persistence, env mapping, knob file writing, and UI panel rendering.
# ============================================================================
import json
import os
import tkinter as tk
from tkinter import ttk

try:
    from .i18n import tr
    from .overlay_win32 import (_u32, GA_ROOT, set_noactivate_for_hwnd,
                                fix_popdown)
except (ImportError, ValueError):
    from i18n import tr
    from overlay_win32 import (_u32, GA_ROOT, set_noactivate_for_hwnd,
                               fix_popdown)


PHASE30_DEFAULTS = {
    "intensity": 1.0,
    "local_tone": 1.0,
    "local_structure": 1.0,
    "skin_structure": -1.0,
    "auto_mask": False,
    "style_index": 0,
    "color_strength": 1.0,
    # Engine-level knobs written into the same file (see module docstring).
    # write_phase30_knobs backfills any the caller did not supply, so the
    # file always carries the full set.
    "gain": 1.0,
    "blend": 1.0,
    "scale": 1.0,
    "sharpness": 0.8,
    "frame_gen": 0,         # match config default False: a writer omitting
                            # the key must not silently turn FG ON
}

# The 7 compose-side vendor knobs (the Phase30Panel set). Everything else in
# PHASE30_DEFAULTS is a pass-through engine knob for the live file - reset
# paths and the panel keep to the vendor list so "Reset" does not also reset
# render scale / blend / frame generation.
PHASE30_VENDOR_KEYS = (
    "intensity", "local_tone", "local_structure", "skin_structure",
    "auto_mask", "style_index", "color_strength",
)

PHASE30_KNOB_FILE = os.path.join(os.environ.get("TEMP", "."), "m13_phase30_knobs.json")


def write_phase30_knobs(params):
    """Write Phase 30 parameters to JSON knob file for C ABI runtime."""
    try:
        data = dict(PHASE30_DEFAULTS)
        if params:
            data.update({k: v for k, v in params.items() if k in PHASE30_DEFAULTS})
        tmp = PHASE30_KNOB_FILE + ".tmp"
        with open(tmp, "w", encoding="ascii") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp, PHASE30_KNOB_FILE)
        return PHASE30_KNOB_FILE
    except OSError:
        return None



def phase30_env_dict(params):
    """Generate environment variables dictionary for game launch injection."""
    p = dict(PHASE30_DEFAULTS)
    if params:
        p.update({k: v for k, v in params.items() if k in PHASE30_DEFAULTS})
    inten = float(p.get("intensity", 1.0))
    ltone = float(p.get("local_tone", 1.0))
    lstruct = float(p.get("local_structure", 1.0))
    sstruct = float(p.get("skin_structure", -1.0))
    amask = 1 if p.get("auto_mask", False) else 0
    sidx = int(p.get("style_index", 0))
    cstrength = float(p.get("color_strength", 1.0))
    # *_GAIN envs are deliberately NOT set here: gain is an engine knob
    # (residual multiplier), pushed by the gain-aware paths (set_gain env
    # writes, launch env) - this dict carries only the Phase30 vendor
    # params so a Phase30 update can never pin gain back to a default.
    return {
        "DLSS5_INTENSITY": "%.3f" % inten,
        "NR_LAYER_INTENSITY": "%.3f" % inten,
        "M12_INTENSITY": "%.3f" % inten,
        "DLSS5_LOCAL_TONE": "%.3f" % ltone,
        "NR_LAYER_LOCAL_TONE": "%.3f" % ltone,
        "M12_LOCAL_TONE": "%.3f" % ltone,
        "DLSS5_LOCAL_STRUCTURE": "%.3f" % lstruct,
        "NR_LAYER_LOCAL_STRUCTURE": "%.3f" % lstruct,
        "M12_LOCAL_STRUCTURE": "%.3f" % lstruct,
        "DLSS5_SKIN_STRUCTURE": "%.3f" % sstruct,
        "NR_LAYER_SKIN_STRUCTURE": "%.3f" % sstruct,
        "M12_SKIN_STRUCTURE": "%.3f" % sstruct,
        "DLSS5_AUTO_MASK": str(amask),
        "NR_LAYER_AUTO_MASK": str(amask),
        "M12_AUTO_MASK": str(amask),
        "DLSS5_STYLE_INDEX": str(sidx),
        "NR_LAYER_STYLE_INDEX": str(sidx),
        "M12_STYLE_INDEX": str(sidx),
        "DLSS5_COLOR_STRENGTH": "%.3f" % cstrength,
        "NR_LAYER_COLOR_STRENGTH": "%.3f" % cstrength,
        "M12_COLOR_STRENGTH": "%.3f" % cstrength,
    }


class ToolTip:
    """Lightweight hover tooltip for Tkinter widgets."""

    def __init__(self, widget, text_fn):
        self.widget = widget
        self.text_fn = text_fn if callable(text_fn) else (lambda: text_fn)
        self.tipwin = None
        self.widget.bind("<Enter>", self.show, add="+")
        self.widget.bind("<Leave>", self.hide, add="+")

    def show(self, _e=None):
        text = self.text_fn()
        if not text or self.tipwin:
            return
        x = self.widget.winfo_rootx() + 15
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 5
        self.tipwin = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_attributes("-topmost", True)
        # Game Bar semantics: a tooltip is a transient TOOL window of the
        # panel, never an activation vector - a bare Toplevel map could
        # steal the game's foreground (exclusive fullscreen minimizes).
        # transient() ties z-order to the owner; the top-level HWND gets
        # WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW applied three ways because Tk
        # re-writes GWL_EXSTYLE while mapping a transient toplevel: once
        # pre-map on the already-created HWND, once after update_idletasks
        # forces the map (Tk's restyle lands first, ours last), and once
        # more at idle as a belt for Tk builds that restyle later.
        try:
            tw.transient(self.widget.winfo_toplevel())
        except tk.TclError:
            pass

        def _harden():
            try:
                th = _u32.GetAncestor(tw.winfo_id(), GA_ROOT) \
                    or tw.winfo_id()
                if th:
                    set_noactivate_for_hwnd(th)
            except Exception:
                pass
        _harden()                            # pre-map, if HWND exists
        tw.geometry("+%d+%d" % (x, y))
        lbl = tk.Label(tw, text=text, justify="left",
                       bg="#1f2430", fg="#e6e9f0",
                       relief="solid", bd=1,
                       font=("Segoe UI", 8), padx=6, pady=4)
        lbl.pack()
        try:
            tw.update_idletasks()            # force the map + Tk restyle
        except tk.TclError:
            pass
        _harden()                            # ours lands last
        tw.after_idle(_harden)

    def hide(self, _e=None):
        if self.tipwin:
            try:
                self.tipwin.destroy()
            except tk.TclError:
                pass
            self.tipwin = None


class Phase30Panel(tk.Frame):
    """Reusable widget with the 7 Phase 30 vendor controls and reset button."""

    def __init__(self, parent, initial_values=None, on_change=None,
                 on_reset=None, bg="#161a23", fg="#e6e9f0", accent="#5b8cff",
                 panel2="#1f2430", muted="#8a91a5", compact=False):
        super().__init__(parent, bg=bg)
        self.bg = bg
        self.fg = fg
        self.accent = accent
        self.panel2 = panel2
        self.muted = muted
        self.compact = compact
        self.on_change = on_change or (lambda k, v: None)
        self.on_reset = on_reset or (lambda: None)

        self._vals = dict(PHASE30_DEFAULTS)
        if initial_values:
            self._vals.update({k: v for k, v in initial_values.items()
                               if k in PHASE30_DEFAULTS})

        self._debounce_ids = {}
        self._dragging = set()
        self._build()

    def _build(self):
        # Header strip: title + Reset button
        head = tk.Frame(self, bg=self.bg)
        head.pack(fill="x", pady=(2, 6))
        title = tk.Label(head, text=tr("p30_title"), bg=self.bg, fg=self.accent,
                         font=("Segoe UI", 9, "bold"))
        title.pack(side="left")

        btn_reset = tk.Button(
            head, text=tr("p30_reset"), font=("Segoe UI", 8, "bold"),
            bg=self.panel2, fg=self.fg, activebackground="#3d2327",
            activeforeground="#f85149", relief="flat", bd=0, padx=8, pady=2,
            cursor="hand2", command=self.reset_defaults)
        btn_reset.pack(side="right")
        ToolTip(btn_reset, lambda: tr("p30_reset_tip"))

        # 1. Intensity (0.0..2.0, public baseline 1.0)
        self._var_intensity = tk.DoubleVar(value=float(self._vals["intensity"]))
        self.lbl_intensity = self._add_slider(
            tr("p30_intensity"), self._var_intensity, 0.0, 2.0, 0.05,
            lambda: tr("p30_intensity_tip"), "intensity")

        # 2. Local Tone (0.0..2.0, default 1.0)
        self._var_local_tone = tk.DoubleVar(value=float(self._vals["local_tone"]))
        self.lbl_local_tone = self._add_slider(
            tr("p30_local_tone"), self._var_local_tone, 0.0, 2.0, 0.05,
            lambda: tr("p30_local_tone_tip"), "local_tone")

        # 3. Local Structure (0.0..2.0, public baseline 1.0)
        self._var_local_structure = tk.DoubleVar(value=float(self._vals["local_structure"]))
        self.lbl_local_structure = self._add_slider(
            tr("p30_local_structure"), self._var_local_structure, 0.0, 2.0, 0.05,
            lambda: tr("p30_local_structure_tip"), "local_structure")

        # 4. Skin Structure (-1.0..2.0, default 2.0; -1.0 = follow local)
        self._var_skin_structure = tk.DoubleVar(value=float(self._vals["skin_structure"]))
        self.lbl_skin_structure = self._add_slider(
            tr("p30_skin_structure"), self._var_skin_structure, -1.0, 2.0, 0.05,
            lambda: tr("p30_skin_structure_tip"), "skin_structure",
            custom_fmt=lambda v: tr("p30_skin_follow") if abs(v - (-1.0)) < 0.02 else ("%.2f" % v))

        # 5. Color Strength (0.0..2.0, default 1.0)
        self._var_color_strength = tk.DoubleVar(value=float(self._vals["color_strength"]))
        self.lbl_color_strength = self._add_slider(
            tr("p30_color_strength"), self._var_color_strength, 0.0, 2.0, 0.05,
            lambda: tr("p30_color_strength_tip"), "color_strength")

        # Row with Style Index + Auto Mask toggle
        row_opts = tk.Frame(self, bg=self.bg)
        row_opts.pack(fill="x", pady=(4, 2))

        # Auto Mask toggle
        self._var_auto_mask = tk.BooleanVar(value=bool(self._vals["auto_mask"]))
        chk_mask = tk.Checkbutton(
            row_opts, text=tr("p30_auto_mask"), variable=self._var_auto_mask,
            bg=self.bg, fg=self.fg, selectcolor=self.panel2,
            activebackground=self.bg, activeforeground=self.fg,
            font=("Segoe UI", 8, "bold"), cursor="hand2",
            command=self._on_mask_toggle)
        chk_mask.pack(side="left")
        ToolTip(chk_mask, lambda: tr("p30_auto_mask_tip"))

        # Style Index
        right_box = tk.Frame(row_opts, bg=self.bg)
        right_box.pack(side="right")
        lbl_style = tk.Label(right_box, text=tr("p30_style_index"), bg=self.bg,
                             fg=self.muted, font=("Segoe UI", 8))
        lbl_style.pack(side="left", padx=(0, 4))
        ToolTip(lbl_style, lambda: tr("p30_style_index_tip"))

        self._style_labels = [tr("p30_style_0"), tr("p30_style_1"), tr("p30_style_2")]
        init_style_idx = int(self._vals["style_index"])
        init_style_lbl = self._style_labels[min(max(init_style_idx, 0), 2)]
        self._var_style = tk.StringVar(value=init_style_lbl)
        self.cb_style = ttk.Combobox(
            right_box, textvariable=self._var_style, state="readonly",
            values=self._style_labels, width=15 if self.compact else 18)
        self.cb_style.pack(side="left")
        self.cb_style.bind("<<ComboboxSelected>>", self._on_style_selected)
        # Same hardening as the overlay's scale combobox: the ttk popdown
        # is its own toplevel and must not become an activation vector.
        self.cb_style.bind("<Button-1>", fix_popdown(self.cb_style),
                           add="+")

    def _add_slider(self, label_text, var, min_v, max_v, res, tip_fn, key,
                    custom_fmt=None):
        pad_y = 1 if self.compact else 2
        row = tk.Frame(self, bg=self.bg)
        row.pack(fill="x", pady=(pad_y, 0))

        lbl = tk.Label(row, text=label_text, bg=self.bg, fg=self.fg,
                       font=("Segoe UI", 8, "bold"))
        lbl.pack(side="left")
        ToolTip(lbl, tip_fn)

        fmt_fn = custom_fmt or (lambda v: "%.2f" % v)
        val_lbl = tk.Label(row, text=fmt_fn(var.get()), bg=self.bg, fg=self.accent,
                           font=("Consolas", 9, "bold"), width=7)
        val_lbl.pack(side="right")

        def _update_disp(val):
            try:
                fval = float(val)
            except ValueError:
                fval = var.get()
            val_lbl.config(text=fmt_fn(fval))
            self._schedule_push(key, fval)

        scale = tk.Scale(
            self, from_=min_v, to=max_v, resolution=res, orient="horizontal",
            variable=var, command=_update_disp,
            bg=self.bg, fg=self.fg, troughcolor=self.panel2,
            highlightthickness=0, bd=0, activebackground=self.accent,
            showvalue=False, sliderrelief="flat")
        scale.pack(fill="x", pady=(0, pad_y))

        scale.bind("<ButtonPress-1>", lambda _e: self._dragging.add(key))
        scale.bind("<ButtonRelease-1>", lambda _e: self._dragging.discard(key))
        ToolTip(scale, tip_fn)
        return val_lbl

    def _schedule_push(self, key, val):
        if key in self._debounce_ids:
            self.after_cancel(self._debounce_ids[key])
        self._debounce_ids[key] = self.after(200, lambda: self._do_push(key, val))

    def _do_push(self, key, val):
        self._debounce_ids.pop(key, None)
        self._vals[key] = val
        self.on_change(key, val)

    def _on_mask_toggle(self):
        val = bool(self._var_auto_mask.get())
        self._vals["auto_mask"] = val
        self.on_change("auto_mask", val)

    def _on_style_selected(self, _e=None):
        txt = self._var_style.get()
        idx = 0
        for i, s in enumerate(self._style_labels):
            if s == txt:
                idx = i
                break
        self._vals["style_index"] = idx
        self.on_change("style_index", idx)

    def reset_defaults(self):
        """Reset all parameters to Phase 30 defaults."""
        self.set_values(PHASE30_DEFAULTS)
        self.on_reset()

    def set_values(self, values):
        """Update widget displays from external values."""
        if not values:
            return
        if "intensity" in values and "intensity" not in self._dragging:
            v = float(values["intensity"])
            self._var_intensity.set(v)
            self.lbl_intensity.config(text="%.2f" % v)
            self._vals["intensity"] = v
        if "local_tone" in values and "local_tone" not in self._dragging:
            v = float(values["local_tone"])
            self._var_local_tone.set(v)
            self.lbl_local_tone.config(text="%.2f" % v)
            self._vals["local_tone"] = v
        if "local_structure" in values and "local_structure" not in self._dragging:
            v = float(values["local_structure"])
            self._var_local_structure.set(v)
            self.lbl_local_structure.config(text="%.2f" % v)
            self._vals["local_structure"] = v
        if "skin_structure" in values and "skin_structure" not in self._dragging:
            v = float(values["skin_structure"])
            self._var_skin_structure.set(v)
            txt = tr("p30_skin_follow") if abs(v - (-1.0)) < 0.02 else ("%.2f" % v)
            self.lbl_skin_structure.config(text=txt)
            self._vals["skin_structure"] = v
        if "color_strength" in values and "color_strength" not in self._dragging:
            v = float(values["color_strength"])
            self._var_color_strength.set(v)
            self.lbl_color_strength.config(text="%.2f" % v)
            self._vals["color_strength"] = v
        if "auto_mask" in values:
            b = bool(values["auto_mask"])
            self._var_auto_mask.set(b)
            self._vals["auto_mask"] = b
        if "style_index" in values:
            idx = int(values["style_index"])
            idx = min(max(idx, 0), 2)
            self._var_style.set(self._style_labels[idx])
            self._vals["style_index"] = idx

    def get_values(self):
        return dict(self._vals)

    def is_dragging(self):
        return bool(self._dragging)
