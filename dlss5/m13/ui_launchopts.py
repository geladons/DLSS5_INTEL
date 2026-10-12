# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.ui_launchopts - per-game "Launch options" dialog: windowed mode with
# a flag template per engine + resolution, and free-form extra arguments.
# Opened from the games-tab extras strip ("Launch options..."); persists
# window / window_w / window_h / window_template / launch_args into the
# game's meta (registered games) or the tab's pending_opts dict (cards
# not yet enabled - the Enable/Play paths carry it into the new meta).
# ============================================================================
import os
import tkinter as tk
from tkinter import ttk

from . import gamelaunch
from .i18n import tr

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
BORDER = "#2a3040"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"

# Meta keys the dialog owns - kept in one place so the tab can carry the
# same set across a re-register (add_game writes a fresh meta).
META_KEYS = ("window", "window_w", "window_h", "window_template",
             "launch_args")

_RES_MIN, _RES_MAX = 320, 7680


def collect_opts(meta):
    """Dialog field values for a meta dict (stored or pending)."""
    meta = meta if isinstance(meta, dict) else {}
    return {
        "window": bool(meta.get("window")),
        "window_w": meta.get("window_w") or 1280,
        "window_h": meta.get("window_h") or 720,
        "window_template": meta.get("window_template"),
        "launch_args": meta.get("launch_args") or "",
    }


class LaunchOptionsDialog:
    """Modal-ish Toplevel for one exe. On OK the assembled opts go to
    tab.apply_launch_opts(exe, opts); nothing touches cfg before that."""

    def __init__(self, tab, exe):
        self.tab = tab
        self.exe = exe
        meta = (tab.ctl.cfg.get("games") or {}).get(exe) or \
            tab.pending_opts.get(exe) or {}
        o = collect_opts(meta)

        top = self.top = tk.Toplevel(tab.tab_frame.winfo_toplevel())
        top.title("%s - %s" % (tr("lo_title"), os.path.basename(exe)))
        top.configure(bg=BG)
        top.transient(tab.tab_frame.winfo_toplevel())
        top.resizable(False, False)

        body = tk.Frame(top, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        body.pack(padx=10, pady=10, fill="both")
        inner = tk.Frame(body, bg=PANEL)
        inner.pack(padx=12, pady=10, fill="both")

        self.window_var = tk.BooleanVar(value=o["window"])
        ttk.Checkbutton(inner, text=tr("lo_windowed"),
                        variable=self.window_var,
                        style="Panel.TCheckbutton",
                        command=self._update).pack(anchor="w")

        res = tk.Frame(inner, bg=PANEL)
        res.pack(anchor="w", pady=(8, 0))
        tk.Label(res, text=tr("lo_width"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        self.w_var = tk.StringVar(value=str(o["window_w"]))
        tk.Spinbox(res, textvariable=self.w_var, from_=_RES_MIN, to=_RES_MAX,
                   width=6, bg=PANEL2, fg=FG, buttonbackground=PANEL2,
                   insertbackground=FG,
                   command=self._update).pack(side="left", padx=(4, 12))
        tk.Label(res, text=tr("lo_height"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        self.h_var = tk.StringVar(value=str(o["window_h"]))
        tk.Spinbox(res, textvariable=self.h_var, from_=_RES_MIN, to=_RES_MAX,
                   width=6, bg=PANEL2, fg=FG, buttonbackground=PANEL2,
                   insertbackground=FG,
                   command=self._update).pack(side="left", padx=(4, 0))
        self.w_var.trace_add("write", lambda *_a: self._update())
        self.h_var.trace_add("write", lambda *_a: self._update())

        tpl_row = tk.Frame(inner, bg=PANEL)
        tpl_row.pack(anchor="w", pady=(8, 0), fill="x")
        tk.Label(tpl_row, text=tr("lo_template"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        # label -> key map; "Custom" appends after the known templates.
        self._tpl_labels = [(lbl, key) for key, (lbl, _a)
                            in gamelaunch.KNOWN_TEMPLATES.items()]
        self._tpl_labels.append((tr("lo_custom"), "custom"))
        key = o["window_template"]
        if not key:
            # Legacy meta with hand-written args: open as Custom so OK
            # keeps that exact command line; auto-suggest only fills a
            # truly empty slot.
            key = "custom" if o["launch_args"].strip() else \
                gamelaunch.suggest_template(exe)
        label = next((lbl for lbl, k in self._tpl_labels if k == key),
                     tr("lo_custom"))
        self.tpl_var = tk.StringVar(value=label)
        self.tpl_box = ttk.Combobox(
            tpl_row, textvariable=self.tpl_var, width=44, state="readonly",
            values=[lbl for lbl, _k in self._tpl_labels])
        self.tpl_box.pack(side="left", padx=(6, 0))
        self.tpl_box.bind("<<ComboboxSelected>>",
                          lambda _e: self._update())

        self.hint_lbl = tk.Label(inner, text=tr("lo_hint_noflag"),
                                 bg=PANEL, fg=MUTED, font=("Segoe UI", 7),
                                 wraplength=430, justify="left")

        self.extra_lbl = tk.Label(inner, text=tr("lo_extra"), bg=PANEL,
                                  fg=FG, font=("Segoe UI", 8))
        self.extra_lbl.pack(anchor="w", pady=(8, 0))
        self.args_text = tk.Text(inner, width=56, height=3, bg=PANEL2, fg=FG,
                                 insertbackground=FG, relief="flat",
                                 font=("Consolas", 8))
        self.args_text.pack(anchor="w", pady=(2, 0))
        if o["launch_args"]:
            self.args_text.insert("1.0", o["launch_args"])
        self.args_text.bind("<KeyRelease>", lambda _e: self._update())

        tk.Label(inner, text=tr("lo_preview"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(anchor="w", pady=(8, 0))
        self.preview = tk.Entry(inner, width=64, bg=PANEL2, fg=MUTED,
                                readonlybackground=PANEL2, relief="flat",
                                font=("Consolas", 8), state="readonly")
        self.preview.pack(anchor="w", pady=(2, 0))

        btns = tk.Frame(inner, bg=PANEL)
        btns.pack(anchor="e", pady=(10, 0))
        ttk.Button(btns, text=tr("ok"), style="Accent.TButton",
                   command=self._ok).pack(side="left", padx=(0, 6))
        ttk.Button(btns, text=tr("cancel"),
                   command=top.destroy).pack(side="left")

        self._update()
        top.update_idletasks()
        # Center over the parent, then grab so the manager stays usable
        # only after this dialog closes.
        par = tab.tab_frame.winfo_toplevel()
        x = par.winfo_rootx() + (par.winfo_width() - top.winfo_width()) // 2
        y = par.winfo_rooty() + (par.winfo_height() - top.winfo_height()) // 2
        top.geometry("+%d+%d" % (max(x, 0), max(y, 0)))
        top.grab_set()
        top.focus_set()

    # ------------------------------------------------------------ helpers --
    def _int(self, var, default):
        try:
            v = int(var.get())
        except (tk.TclError, ValueError):
            return default
        return min(max(v, _RES_MIN), _RES_MAX)

    def _tpl_key(self):
        label = self.tpl_var.get()
        for lbl, key in self._tpl_labels:
            if lbl == label:
                return key
        return "custom"

    def _opts(self):
        return {
            "window": bool(self.window_var.get()),
            "window_w": self._int(self.w_var, 1280),
            "window_h": self._int(self.h_var, 720),
            "window_template": self._tpl_key(),
            "launch_args": self.args_text.get("1.0", "end-1c").strip(),
        }

    def _update(self):
        o = self._opts()
        args = gamelaunch.build_launch_args(
            {k: o[k] for k in META_KEYS})
        cmd = '"%s"' % self.exe + (" " + " ".join(args) if args else "")
        self.preview.config(state="normal")
        self.preview.delete(0, "end")
        self.preview.insert(0, cmd)
        self.preview.config(state="readonly")
        # The no-flag hint only makes sense while windowed is requested
        # with a template that cannot produce flags.
        if o["window"] and self._tpl_key() == "none":
            self.hint_lbl.pack(anchor="w", pady=(4, 0), before=self.extra_lbl)
        else:
            self.hint_lbl.pack_forget()

    def _ok(self):
        o = self._opts()
        # Write the validated/clamped resolution back into the fields.
        self.w_var.set(str(o["window_w"]))
        self.h_var.set(str(o["window_h"]))
        self.tab.apply_launch_opts(self.exe, o)
        self.top.destroy()
