# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.ui - DLSS 5 Manager main window (tkinter, stdlib only, dark theme).
#
# Design rule (owner feedback): the demo must be HANDS-FREE. On startup the
# app finds the weights itself, registers the Vulkan layers itself, starts
# the daemon itself and scans every fixed drive for games itself. The human
# only picks a game and presses "Enable" / "Launch".
#
# v3 look (owner: "boring and unfriendly"): deep-navy palette, icon cards
# for games, pill checklist with plain-Russian hints, a startup SPLASH with
# a neural-net doodle and joke loading lines (splash.py), and a hidden
# easter egg (click the logo five times).
#
# Layout:
#   header     logo + title + daemon state + gain + in-game overlay button
#   checklist  auto-driven setup steps (weights/layers/daemon/games)
#   notebook   Games (icon cards) | Screen | Settings
#   log pane   threaded tails of m11d/m12/layer/m8blive logs
#
# RESPONSIVENESS RULE: this file NEVER calls a blocking probe. All state
# comes from ctl.snapshot() (StateMonitor thread); every action goes through
# ctl.submit() (ActionWorker thread). The Tk thread only renders.
# ============================================================================
import os
import queue
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from .controller import M13Controller
from .i18n import (set_language, tr, scale_preset_labels,
                   scale_to_preset_label, preset_label_to_scale)
from .logtail import LogHub
from .overlay import ControlOverlay, hotkey_label
from .phase30 import Phase30Panel, PHASE30_DEFAULTS
from .splash import Splash
from .ui_games import GamesTab

POLL_MS = 500

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
BORDER = "#2a3040"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
ACCENT2 = "#9a6bff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

TAG_COLORS = {"m11d": GREEN, "m12": "#58a6ff", "layer": AMBER,
              "m8blive": MUTED, "mgr": FG}

EASTER_EGG_CLICKS = 5


class ManagerUI:
    def __init__(self):
        self.ctl = M13Controller()
        set_language(self.ctl.cfg.get("language"))
        # Own taskbar identity + icon (pythonw would otherwise show the
        # generic Python icon and group with every other pythonw window).
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
                "DLSS5.Manager")
        except (AttributeError, OSError):
            pass
        self.root = tk.Tk()
        try:
            self.root.iconbitmap(default=os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "icon.ico"))
        except tk.TclError:
            pass
        self.root.title(tr("app_title"))
        self.root.geometry("1024x760")
        self.root.minsize(1024, 760)
        self.root.configure(bg=BG)
        self.root.bind("<FocusIn>", self._on_focus_in)
        self.logq = queue.Queue()
        self.hub = LogHub(self._enqueue_log)
        self._egg_clicks = 0
        self._style()
        self._build()
        self.ctl.start_worker(self._on_action_result)
        self.hub.start()
        self.root.after(200, self._drain_log)
        self.root.after(POLL_MS, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        # hands-free bring-up + cached or first game scan (worker thread)
        self.ctl.submit(self.ctl.autosetup)
        if not self.games_tab.scanned:
            self.games_tab.scan_start()
        self._splash()

    # ------------------------------------------------------------ splash ---
    def _splash(self):
        """Startup easter egg: a small splash over the (withdrawn) main
        window while the autosetup runs; the main window appears when the
        minimum show time passed."""
        self.root.withdraw()
        splash = Splash(self.root)
        splash.start()

        def reveal():
            if splash.ready_to_close():
                splash.close()
                self.root.deiconify()
                self.root.lift()
                return
            self.root.after(250, reveal)
        self.root.after(250, reveal)

    # ------------------------------------------------------------- style ---
    def _style(self):
        s = ttk.Style(self.root)
        try:
            s.theme_use("clam")
        except tk.TclError:
            pass
        s.configure(".", background=BG, foreground=FG,
                    font=("Segoe UI", 9))
        s.configure("TFrame", background=BG)
        s.configure("TLabel", background=BG, foreground=FG)
        s.configure("Muted.TLabel", background=BG, foreground=MUTED)
        s.configure("TLabelframe", background=BG, foreground=FG,
                    bordercolor=BORDER)
        s.configure("TLabelframe.Label", background=BG, foreground=FG)
        s.configure("TButton", background=PANEL, foreground=FG,
                    padding=(12, 6), borderwidth=0)
        s.map("TButton", background=[("active", PANEL2)],
              foreground=[("disabled", "#565b66")])
        s.configure("Accent.TButton", background=ACCENT, foreground="#ffffff",
                    font=("Segoe UI", 9, "bold"))
        s.map("Accent.TButton", background=[("active", "#3a70d6")])
        s.configure("Danger.TButton", background="#3d2327", foreground=RED)
        s.map("Danger.TButton", background=[("active", "#523035")])
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", background=PANEL, foreground=MUTED,
                    padding=(18, 8))
        s.map("TNotebook.Tab", background=[("selected", PANEL2)],
              foreground=[("selected", FG)])
        s.configure("TEntry", fieldbackground=PANEL, foreground=FG,
                    insertcolor=FG, bordercolor=BORDER)
        s.configure("Horizontal.TScale", background=BG, troughcolor=PANEL)
        s.configure("TCheckbutton", background=BG, foreground=FG)
        s.map("TCheckbutton", background=[("active", BG)])
        s.configure("TCombobox", fieldbackground=PANEL, foreground=FG,
                    background=PANEL)
        s.configure("Vertical.TScrollbar", background=PANEL,
                    troughcolor=BG, borderwidth=0, arrowcolor=FG)

    # ------------------------------------------------------------- build ---
    def _build(self):
        self.overlay = ControlOverlay(
            self.root, self.ctl, on_log=lambda m: self.log("mgr", m))
        self.knob = self.overlay          # back-compat alias
        self._build_header()
        self._build_checklist()

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=10, pady=(4, 0))
        self.nb = nb
        self.games_tab = GamesTab(self, nb)
        self._tab_screen(nb)
        self._tab_settings(nb)

        self.logtxt = ScrolledText(self.root, height=8, state="disabled",
                                   font=("Consolas", 9), bg="#0a0c11",
                                   fg=FG, insertbackground=FG,
                                   relief="flat", bd=6)
        self.logtxt.pack(fill="both", expand=False, padx=10, pady=6)
        for tag, color in TAG_COLORS.items():
            self.logtxt.tag_config(tag, foreground=color)

    def _build_header(self):
        head = tk.Frame(self.root, bg=BG)
        head.pack(fill="x", padx=12, pady=(12, 4))
        self.logo = tk.Canvas(head, width=30, height=30, bg=BG,
                              highlightthickness=0, cursor="hand2")
        self.logo.pack(side="left", padx=(0, 8))
        self._draw_logo(ACCENT)
        self.logo.bind("<Button-1>", self._logo_click)
        title = tk.Frame(head, bg=BG)
        title.pack(side="left")
        self.title_lbl = tk.Label(title, text=tr("app_title"), bg=BG, fg=FG,
                                  font=("Segoe UI", 14, "bold"))
        self.title_lbl.pack(anchor="w")
        tk.Label(title, text=tr("app_subtitle"), bg=BG, fg=MUTED,
                 font=("Segoe UI", 8)).pack(anchor="w")
        ttk.Button(head, text=tr("overlay_button", hotkey_label(
            self.ctl.cfg.get("overlay_hotkey"))),
            command=self.overlay.toggle).pack(side="right")

        bar = tk.Frame(self.root, bg=PANEL,
                       highlightthickness=1, highlightbackground=BORDER)
        bar.pack(fill="x", padx=12, pady=4)
        status_box = tk.Frame(bar, bg=PANEL)
        status_box.pack(fill="x", padx=10, pady=8)
        self.dot = tk.Label(status_box, text="●", font=("Segoe UI", 10),
                            bg=PANEL, fg=GREEN)
        self.dot.pack(side="left")
        self.st_engine = tk.Label(status_box, text=tr("engine_active"),
                                  bg=PANEL, fg=GREEN,
                                  font=("Segoe UI", 9, "bold"))
        self.st_engine.pack(side="left", padx=(6, 8))
        self.st_daemon = self.st_engine  # back-compat alias
        self.daemon_info = tk.Label(status_box,
                                    text="Direct GPU (0 ms TCP lag)",
                                    bg=PANEL, fg=MUTED,
                                    font=("Segoe UI", 8))
        self.daemon_info.pack(side="left", padx=(4, 0))

        self.scale_var = tk.StringVar(
            value=scale_to_preset_label(self.ctl.cfg.get("scale")))
        self.framegen_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("frame_gen")))

    def _draw_logo(self, color):
        self.logo.delete("all")
        self.logo.create_polygon(15, 2, 28, 15, 15, 28, 2, 15,
                                 fill=color, outline="")
        self.logo.create_polygon(15, 8, 22, 15, 15, 22, 8, 15,
                                 fill=BG, outline="")

    def _logo_click(self, _e):
        """Hidden easter egg: five clicks on the diamond."""
        self._egg_clicks += 1
        if self._egg_clicks < EASTER_EGG_CLICKS:
            return
        self._egg_clicks = 0
        self.title_lbl.config(text=tr("egg_title"))
        self.log("mgr", tr("egg_log"))
        colors = (ACCENT, ACCENT2, GREEN, AMBER, RED, "#ff6ec7")

        def flash(i=0):
            if i >= 12:
                self._draw_logo(ACCENT)
                self.title_lbl.config(text=tr("app_title"))
                return
            self._draw_logo(colors[i % len(colors)])
            self.root.after(150, lambda: flash(i + 1))
        flash()

    def _build_checklist(self):
        box = tk.Frame(self.root, bg=PANEL,
                       highlightthickness=1, highlightbackground=BORDER)
        box.pack(fill="x", padx=12, pady=4)
        row = tk.Frame(box, bg=PANEL)
        row.pack(fill="x", padx=10, pady=(8, 2))
        tk.Label(row, text=tr("autosetup"), bg=PANEL, fg=MUTED,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.steps = {}
        for key in ("weights", "layers", "engine", "game"):
            lbl = tk.Label(row, text=tr("step_" + key), bg=PANEL, fg=MUTED,
                           font=("Segoe UI", 9), padx=10)
            lbl.pack(side="left")
            self.steps[key] = lbl
        self.hint = tk.Label(box, text="", bg=PANEL, fg=AMBER,
                             font=("Segoe UI", 9), anchor="w")
        self.hint.pack(fill="x", padx=10, pady=(0, 8))

    # --------------------------------------------------------- Screen tab --
    def _tab_screen(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text=tr("tab_screen"))
        tk.Label(t, text="Direct DDA Vulkan Overlay (m8blive.exe) - Direct GPU Pipeline (No TCP Daemon)",
                 bg=BG, fg=ACCENT, font=("Segoe UI", 9, "bold")).pack(anchor="w", pady=(0, 4))
        ttk.Label(t, text=tr("screen_note"),
                  style="Muted.TLabel", wraplength=900,
                  justify="left").pack(anchor="w")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=8)
        ttk.Button(row, text=tr("screen_start"),
                   style="Accent.TButton",
                   command=self._screen_start).pack(side="left")
        ttk.Button(row, text=tr("screen_stop"), style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.screen_stop)).pack(side="left", padx=6)
        ttk.Label(row, text=tr("screen_hotkeys"),
                  style="Muted.TLabel").pack(side="left", padx=14)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=(4, 2))
        ttk.Label(row2, text=tr("screen_window_mode")).pack(side="left")
        self.win_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.win_var, width=30).pack(
            side="left", padx=6)
        ttk.Button(row2, text=tr("screen_window_start"),
                   command=self._screen_window_start).pack(side="left")
        self.screen_info = ttk.Label(t, text=tr("screen_state_off"),
                                     style="Muted.TLabel")
        self.screen_info.pack(anchor="w", pady=6)
        self.screen_tail = ttk.Label(
            t, text=tr("screen_warmup"),
            style="Muted.TLabel", wraplength=900, justify="left")
        self.screen_tail.pack(anchor="w")

    def _screen_start(self):
        self.ctl.submit(self.ctl.screen_start)

    def _screen_window_start(self):
        self.ctl.submit(lambda: self.ctl.screen.start_window(
            self.win_var.get(), self.ctl.cfg.get("gain"),
            float(self.ctl.cfg.get("blend") or 1.0)))

    # ------------------------------------------------------- Settings tab --
    def _tab_settings(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text=tr("tab_settings"))
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=4, fill="x")
        ttk.Label(row, text=tr("weights_label")).pack(side="left")
        self.weights_var = tk.StringVar(
            value=self.ctl.cfg.get("weights_path") or "")
        ttk.Entry(row, textvariable=self.weights_var,
                  width=64).pack(side="left", padx=6)

        def browse_weights():
            p = filedialog.askopenfilename(
                title="dlssnr-logical.safetensors",
                filetypes=[("safetensors", "*.safetensors"), ("all", "*.*")])
            if p:
                self.weights_var.set(p)
                self.ctl.cfg.set("weights_path", p)
                self.log("mgr", "weights path set: %s" % p)
        ttk.Button(row, text=tr("browse"), command=browse_weights).pack(
            side="left")
        ttk.Button(row, text=tr("save"), command=lambda: (
            self.ctl.cfg.set("weights_path", self.weights_var.get()),
            self.log("mgr", "weights path saved"))).pack(side="left", padx=6)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=6)
        ttk.Button(row2, text=tr("layers_register"),
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_register)).pack(side="left")
        ttk.Button(row2, text=tr("layers_unregister"),
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_unregister)).pack(side="left", padx=6)
        self.layers_info = ttk.Label(row2, text=tr("layers_unknown"),
                                     style="Muted.TLabel")
        self.layers_info.pack(side="left", padx=10)

        row2b = ttk.Frame(t)
        row2b.pack(anchor="w", pady=4)
        ttk.Button(row2b, text=tr("clean_btn"), style="Danger.TButton",
                   command=self._clean_traces).pack(side="left")
        ttk.Label(row2b, text=tr("clean_hint"),
                  style="Muted.TLabel").pack(side="left", padx=10)

        lang = ttk.Frame(t)
        lang.pack(anchor="w", pady=6)
        ttk.Label(lang, text=tr("language_label")).pack(side="left")
        self.lang_var = tk.StringVar(
            value="Русский" if self.ctl.cfg.get("language") == "ru"
            else "English")
        lang_box = ttk.Combobox(lang, textvariable=self.lang_var, width=10,
                                state="readonly",
                                values=("English", "Русский"))
        lang_box.pack(side="left", padx=6)

        def lang_changed(_e=None):
            self.ctl.cfg.set("language", "ru"
                             if self.lang_var.get().startswith("Р") else "en")
        lang_box.bind("<<ComboboxSelected>>", lang_changed)
        ttk.Label(lang, text=tr("language_note"),
                  style="Muted.TLabel").pack(side="left")

        eng = ttk.LabelFrame(t, text=tr("scale"), padding=8)
        eng.pack(fill="x", pady=6)
        ttk.Label(eng, text=tr("scale")).pack(side="left", padx=(0, 6))
        self.scale_cb = ttk.Combobox(
            eng, textvariable=self.scale_var, width=24, state="readonly",
            values=scale_preset_labels())
        self.scale_cb.pack(side="left", padx=(0, 14))
        self.scale_cb.bind("<<ComboboxSelected>>", self._scale_selected)
        self.framegen_chk = ttk.Checkbutton(
            eng, text=tr("framegen"), variable=self.framegen_var,
            command=self._framegen_toggled)
        self.framegen_chk.pack(side="left", padx=(0, 14))
        self.fg_hint = ttk.Label(eng, text=tr("framegen_hint"),
                                 style="Muted.TLabel")
        self.fg_hint.pack(side="right")

        opts = ttk.Frame(t)
        opts.pack(anchor="w", pady=6)
        self.freeze_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        ttk.Checkbutton(opts, text=tr("opt_freeze"),
                        variable=self.freeze_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_freeze", bool(self.freeze_var.get()))
                        ).pack(anchor="w")
        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        ttk.Checkbutton(opts, text=tr("opt_autopause"),
                        variable=self.ap_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_autopause", bool(self.ap_var.get()))
                        ).pack(anchor="w")
        self.focus_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_focus")))
        ttk.Checkbutton(opts, text=tr("opt_focus"),
                        variable=self.focus_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_focus", bool(self.focus_var.get()))
                        ).pack(anchor="w")

        p30_wrap = tk.Frame(t, bg=PANEL, highlightthickness=1,
                            highlightbackground=BORDER)
        p30_wrap.pack(fill="x", pady=(10, 6))
        self.p30_panel = Phase30Panel(
            p30_wrap, initial_values=self.ctl.cfg.get_phase30(),
            on_change=self._on_phase30_change,
            on_reset=self._on_phase30_reset,
            bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED)
        self.p30_panel.pack(fill="x", padx=10, pady=8)

        ttk.Label(t, text=tr("hotkey_note",
                             self.ctl.cfg.get("overlay_hotkey")),
                  style="Muted.TLabel").pack(anchor="w", pady=(10, 2))
        ttk.Label(t, text=tr("config_note", self.ctl.cfg.path),
                  style="Muted.TLabel").pack(anchor="w")

    # ----------------------------------------------------------- actions ---
    def _on_action_result(self, ok, msg):
        """Worker-thread callback: marshal into the Tk thread via the queue."""
        self.logq.put(("mgr", msg))

    def _daemon_start(self):
        self.ctl.submit(self.ctl.daemon_start)

    # ------------------------------------------------------- clean traces --
    def _clean_traces(self):
        """'Remove all traces' button: refuse while a game runs, scan on the
        worker, then confirm on the Tk thread."""
        if self.ctl.active_game_pid() is not None:
            messagebox.showwarning(tr("clean_title"), tr("clean_running"))
            return
        self.log("mgr", tr("clean_scanning"))
        self.ctl.submit(self._clean_scan)

    def _clean_scan(self):
        """Worker job: inventory all traces (dll hashing is real I/O - never
        on the Tk thread), then bounce back for the confirmation dialog."""
        try:
            from .cleaner import TraceScanner
            traces = TraceScanner(self.ctl.cfg).scan()
        except Exception as e:
            return False, "clean scan failed: %s" % e
        self.logq.put(("__call__", lambda: self._clean_confirm(traces)))
        return True, tr("clean_scan_done", len(traces))

    def _clean_confirm(self, traces):
        """Tk thread: count summary -> first confirm, then the optional
        'also my settings' confirm, then the removal job on the worker."""
        from .cleaner import count_summary
        c = count_summary(traces)
        n_game = c.get("game_dll", 0) + c.get("game_backup", 0)
        note = tr("clean_foreign_note", c["foreign"]) if c.get("foreign") \
            else ""
        if not messagebox.askyesno(
                tr("clean_title"),
                tr("clean_confirm", n_game, c.get("registry", 0),
                   c.get("temp_file", 0), note)):
            return
        include_cfg = messagebox.askyesno(tr("clean_title"),
                                          tr("clean_ask_config"))
        self.ctl.submit(self._clean_remove, traces, include_cfg)

    def _clean_remove(self, traces, include_cfg):
        """Worker job: delete/restore everything the scan found. Each step
        logs a [clean] line through the log hub; the summary lands in the
        log pane via the worker result callback."""
        from .cleaner import Cleaner

        def _log(m):
            print(m, flush=True)
            self.ctl._worker_log(m)

        ok, summary = Cleaner(self.ctl.cfg, log=_log).remove(
            traces, include_config=include_cfg)
        return ok, tr("clean_done", summary)


    def _on_phase30_change(self, key, val):
        self.ctl.submit(self.ctl.set_phase30, {key: val})
        if hasattr(self, "overlay") and self.overlay:
            self.overlay.sync_phase30({key: val})

    def _on_phase30_reset(self):
        self.ctl.submit(self.ctl.reset_phase30)
        if hasattr(self, "overlay") and self.overlay:
            self.overlay.sync_phase30(PHASE30_DEFAULTS)


    def _scale_selected(self, _e=None):
        lbl = self.scale_var.get()
        s = preset_label_to_scale(lbl)
        self.ctl.submit(self.ctl.set_scale, s)
        if hasattr(self, "overlay") and self.overlay:
            self.overlay.sync_scale(s)

    def _framegen_toggled(self):
        fg = bool(self.framegen_var.get())
        self.ctl.submit(self.ctl.set_framegen, fg)
        if hasattr(self, "overlay") and self.overlay:
            self.overlay.sync_framegen(fg)

    # -------------------------------------------------------------- poll ---
    def _poll(self):
        """Render the monitor snapshot. Zero I/O on this thread."""
        try:
            self.games_tab.scan_drain()
            snap = self.ctl.snapshot()
            self._render(snap)
        except Exception as e:
            self.log("mgr", "poll error: %s" % e)
        self.root.after(POLL_MS, self._poll)

    def _render(self, snap):
        engine_ok = snap.get("engine_ok") and snap.get("weights_ok")
        if engine_ok:
            self.dot.config(text="●", fg=GREEN)
            self.st_engine.config(text=tr("engine_active"), fg=GREEN)
            daemon_pid = snap.get("daemon_pid")
            if daemon_pid:
                self.daemon_info.config(text="TCP daemon PID %s" % daemon_pid)
            else:
                self.daemon_info.config(text="Direct GPU (0 ms TCP lag)")
        else:
            self.dot.config(text="●", fg=AMBER)
            self.st_engine.config(text=tr("engine_inactive"), fg=AMBER)
            self.daemon_info.config(text="")

        lx = snap.get("layers_x64"), snap.get("layers_x86")
        self.layers_info.config(text=tr("layers_state", "OK" if lx[0]
                                        else "NO", "OK" if lx[1] else "NO"))
        screen_up = snap.get("screen_pid") is not None
        self.screen_info.config(
            text=tr("screen_state_up", snap["screen_pid"]) if screen_up
            else tr("screen_state_off"))
        self.games_tab.render(snap)
        if hasattr(self, "p30_panel") and self.p30_panel and snap.get("phase30"):
            self.p30_panel.set_values(snap.get("phase30"))
        self._render_checklist(snap)

    def _render_checklist(self, snap):
        weights = snap.get("weights_ok")
        layers = snap.get("layers_x64") and snap.get("layers_x86")
        engine = snap.get("engine_ok") and weights
        games = snap.get("games") or {}
        game = any(g.get("dll") == "deployed" for g in games.values())
        states = {"weights": weights, "layers": layers, "engine": engine,
                  "game": game}
        for key, ok in states.items():
            base = tr("step_" + key)
            self.steps[key].config(text=("✔ " if ok else "· ") + base,
                                   fg=GREEN if ok else MUTED)
        if not weights:
            hint, color = tr("hint_no_weights"), AMBER
        elif not layers or not engine:
            hint, color = tr("hint_working"), AMBER
        elif not game:
            hint, color = tr("hint_pick_game"), AMBER
        else:
            hint, color = tr("hint_ready", hotkey_label(
                self.ctl.cfg.get("overlay_hotkey"))), GREEN
        self.hint.config(text=hint, fg=color)

    # -------------------------------------------------------------- log ---
    def _enqueue_log(self, tag, line):
        self.logq.put((tag, line))

    def _drain_log(self):
        batch = []
        try:
            while True:
                batch.append(self.logq.get_nowait())
        except queue.Empty:
            pass
        if batch:
            self.logtxt.config(state="normal")
            for tag, line in batch:
                if tag == "__call__":
                    # deferred UI callback marshalled from a worker job
                    try:
                        line()
                    except Exception as e:
                        self.logtxt.insert(
                            "end", "[mgr] ui callback error: %s\n" % e, "mgr")
                    continue
                self.logtxt.insert("end", "[%s] %s\n" % (tag, line), tag)
            self.logtxt.see("end")
            if int(self.logtxt.index("end-1c").split(".")[0]) > 2000:
                self.logtxt.delete("1.0", "1500.0")
            self.logtxt.config(state="disabled")
        self.root.after(200, self._drain_log)

    def log(self, tag, msg):
        self.logq.put((tag, msg))

    def _on_focus_in(self, event=None):
        """When returning to desktop from an exclusive fullscreen game (e.g. 800x600),
        Windows may shrink the UI window. Restore 1024x760 immediately."""
        if event is not None and event.widget != self.root:
            return
        if self.root.winfo_width() < 1000 or self.root.winfo_height() < 740:
            self.root.geometry("1024x760")
            self.root.update_idletasks()
            if hasattr(self, "games_tab") and self.games_tab:
                self.games_tab.cards_fill()

    # ----------------------------------------------------------- startup ---
    def _on_close(self):
        self.overlay.unfreeze()          # never leave a game suspended
        self.ctl.shutdown()
        self.hub.stop()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def main():
    ManagerUI().run()
