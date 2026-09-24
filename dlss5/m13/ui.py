# ============================================================================
# m13.ui - DLSS 5 Manager main window (tkinter, stdlib only, dark theme).
#
# Design rule (owner feedback): the demo must be HANDS-FREE. On startup the
# app finds the weights itself, registers the Vulkan layers itself, starts
# the daemon itself and scans every fixed drive for games itself. The human
# only picks a game and presses "Enable" / "Launch".
#
# Layout:
#   header     title + daemon state + gain + in-game overlay button
#   checklist  auto-driven setup steps (weights/layers/daemon/games)
#   notebook   Games (scanner) | Screen | Settings
#   log pane   threaded tails of m11d/m12/layer/m8blive logs
#
# RESPONSIVENESS RULE: this file NEVER calls a blocking probe. All state
# comes from ctl.snapshot() (StateMonitor thread); every action goes through
# ctl.submit() (ActionWorker thread). The Tk thread only renders.
# ============================================================================
import os
import queue
import time
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

from .controller import M13Controller
from .logtail import LogHub
from .overlay import ControlOverlay, hotkey_label
from .ui_games import GamesTab

POLL_MS = 500

BG = "#16181d"
PANEL = "#22252d"
PANEL2 = "#2a2e37"
FG = "#d7dae0"
MUTED = "#8b919e"
ACCENT = "#4f8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

TAG_COLORS = {"m11d": GREEN, "m12": "#58a6ff", "layer": AMBER,
              "m8blive": MUTED, "mgr": FG}

class ManagerUI:
    def __init__(self):
        self.ctl = M13Controller()
        self.root = tk.Tk()
        self.root.title("DLSS 5 Manager")
        self.root.geometry("980x720")
        self.root.minsize(860, 620)
        self.root.configure(bg=BG)
        self.logq = queue.Queue()
        self.hub = LogHub(self._enqueue_log)
        self._style()
        self._build()
        self.ctl.start_worker(self._on_action_result)
        self.hub.start()
        self.root.after(200, self._drain_log)
        self.root.after(POLL_MS, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        # hands-free bring-up + first game scan
        self.ctl.submit(self.ctl.autosetup)
        self.games_tab.scan_start()

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
                    bordercolor=PANEL2)
        s.configure("TLabelframe.Label", background=BG, foreground=FG)
        s.configure("TButton", background=PANEL, foreground=FG,
                    padding=(10, 5), borderwidth=0)
        s.map("TButton", background=[("active", PANEL2)],
              foreground=[("disabled", "#565b66")])
        s.configure("Accent.TButton", background=ACCENT, foreground="#ffffff",
                    font=("Segoe UI", 9, "bold"))
        s.map("Accent.TButton", background=[("active", "#3a70d6")])
        s.configure("Danger.TButton", background="#3d2327", foreground=RED)
        s.map("Danger.TButton", background=[("active", "#523035")])
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", background=PANEL, foreground=MUTED,
                    padding=(16, 7))
        s.map("TNotebook.Tab", background=[("selected", PANEL2)],
              foreground=[("selected", FG)])
        s.configure("TEntry", fieldbackground=PANEL, foreground=FG,
                    insertcolor=FG, bordercolor=PANEL2)
        s.configure("Horizontal.TScale", background=BG, troughcolor=PANEL)
        s.configure("TCheckbutton", background=BG, foreground=FG)
        s.map("TCheckbutton", background=[("active", BG)])
        s.configure("TCombobox", fieldbackground=PANEL, foreground=FG,
                    background=PANEL)
        s.configure("Treeview", background="#101216", fieldbackground="#101216",
                    foreground=FG, rowheight=22, borderwidth=0)
        s.configure("Treeview.Heading", background=PANEL, foreground=MUTED)
        s.map("Treeview", background=[("selected", "#27324a")])

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

        self.logtxt = ScrolledText(self.root, height=9, state="disabled",
                                   font=("Consolas", 9), bg="#101216",
                                   fg=FG, insertbackground=FG,
                                   relief="flat", bd=6)
        self.logtxt.pack(fill="both", expand=False, padx=10, pady=6)
        for tag, color in TAG_COLORS.items():
            self.logtxt.tag_config(tag, foreground=color)

    def _build_header(self):
        head = tk.Frame(self.root, bg=BG)
        head.pack(fill="x", padx=10, pady=(10, 4))
        tk.Label(head, text="DLSS 5 Manager", bg=BG, fg=FG,
                 font=("Segoe UI", 13, "bold")).pack(side="left")
        self.dot = tk.Label(head, text="?", font=("Segoe UI", 10), bg=BG)
        self.dot.pack(side="left", padx=(18, 4))
        self.st_daemon = tk.Label(head, text="daemon: ?", bg=BG, fg=MUTED,
                                  font=("Segoe UI", 9))
        self.st_daemon.pack(side="left")
        ttk.Button(head, text="In-game overlay (%s)" % hotkey_label(
            self.ctl.cfg.get("overlay_hotkey")),
            command=self.overlay.toggle).pack(side="right")

        bar = tk.Frame(self.root, bg=PANEL)
        bar.pack(fill="x", padx=10, pady=4)
        inner = tk.Frame(bar, bg=PANEL)
        inner.pack(fill="x", padx=8, pady=8)
        self.btn_start = ttk.Button(inner, text="Start daemon",
                                    style="Accent.TButton",
                                    command=self._daemon_start)
        self.btn_start.pack(side="left")
        ttk.Button(inner, text="Stop", style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.daemon_stop)).pack(side="left", padx=6)
        tk.Label(inner, text="gain", bg=PANEL, fg=MUTED).pack(side="left",
                                                              padx=(18, 4))
        self.gain_var = tk.DoubleVar(value=self.ctl.cfg.get("gain"))
        self._suppress_scale = None   # programmatic-set echo suppression:
        self._scale_dragging = False  # Tk fires the scale command DEFERRED
        self._pending_push = None     # (value, time) of an in-flight push
        self.gain_scale = tk.Scale(
            inner, from_=0.0, to=2.0, resolution=0.05, orient="horizontal",
            length=200, variable=self.gain_var, command=self._gain_moved,
            bg=PANEL, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.gain_scale.pack(side="left")
        self.gain_scale.bind("<ButtonPress-1>",
                             lambda _e: setattr(self, "_scale_dragging", True))
        self.gain_scale.bind("<ButtonRelease-1>",
                             lambda _e: setattr(self, "_scale_dragging",
                                                False))
        self.gain_lbl = tk.Label(inner, text="%.2f" % self.gain_var.get(),
                                 bg=PANEL, fg=ACCENT,
                                 font=("Consolas", 10, "bold"), width=5)
        self.gain_lbl.pack(side="left")
        self.daemon_info = tk.Label(inner, text="", bg=PANEL, fg=MUTED,
                                    font=("Segoe UI", 8))
        self.daemon_info.pack(side="right")

    def _build_checklist(self):
        box = tk.Frame(self.root, bg=PANEL)
        box.pack(fill="x", padx=10, pady=4)
        row = tk.Frame(box, bg=PANEL)
        row.pack(fill="x", padx=8, pady=6)
        tk.Label(row, text="Setup (automatic):", bg=PANEL, fg=MUTED,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.steps = {}
        for key, label in (("weights", "Weights"), ("layers", "Layers"),
                           ("daemon", "Daemon"), ("game", "Game ready")):
            lbl = tk.Label(row, text=label, bg=PANEL, fg=MUTED,
                           font=("Segoe UI", 9), padx=10)
            lbl.pack(side="left")
            self.steps[key] = lbl
        self.hint = tk.Label(box, text="", bg=PANEL, fg=AMBER,
                             font=("Segoe UI", 9), anchor="w")
        self.hint.pack(fill="x", padx=8, pady=(0, 6))

    # --------------------------------------------------------- Screen tab --
    def _tab_screen(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text="  Screen  ")
        ttk.Label(t, text="Fullscreen desktop overlay (watch through the "
                          "Moonlight stream). SLOW warm-up at 1440p: weights "
                          "upload ~30 s, first frame up to a minute - it is "
                          "loading, not dead. For a quick demo use window "
                          "mode below.", style="Muted.TLabel", wraplength=860,
                  justify="left").pack(anchor="w")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=8)
        ttk.Button(row, text="Start fullscreen overlay", style="Accent.TButton",
                   command=self._screen_start).pack(side="left")
        ttk.Button(row, text="Stop", style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.screen_stop)).pack(side="left", padx=6)
        ttk.Label(row, text="hotkeys: CTRL+ALT+X hide/show, CTRL+ALT+Q quit",
                  style="Muted.TLabel").pack(side="left", padx=14)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=(4, 2))
        ttk.Label(row2, text="Window mode - title contains:").pack(side="left")
        self.win_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.win_var, width=30).pack(side="left",
                                                                  padx=6)
        ttk.Button(row2, text="Start on window",
                   command=self._screen_window_start).pack(side="left")
        self.screen_info = ttk.Label(t, text="overlay: ?",
                                     style="Muted.TLabel")
        self.screen_info.pack(anchor="w", pady=6)

    def _screen_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self.ctl.submit(self.ctl.screen_start)

    def _screen_window_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self.ctl.submit(lambda: self.ctl.screen.start_window(
            self.win_var.get(), self.ctl.cfg.get("gain")))

    # ------------------------------------------------------- Settings tab --
    def _tab_settings(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text="  Settings  ")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=4, fill="x")
        ttk.Label(row, text="weights (.safetensors):").pack(side="left")
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
        ttk.Button(row, text="Browse...", command=browse_weights).pack(
            side="left")
        ttk.Button(row, text="Save", command=lambda: (
            self.ctl.cfg.set("weights_path", self.weights_var.get()),
            self.log("mgr", "weights path saved"))).pack(side="left", padx=6)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=6)
        ttk.Button(row2, text="Register Vulkan layers (HKCU)",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_register)).pack(side="left")
        ttk.Button(row2, text="Unregister",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_unregister)).pack(side="left", padx=6)
        self.layers_info = ttk.Label(row2, text="layers: ?",
                                     style="Muted.TLabel")
        self.layers_info.pack(side="left", padx=10)

        opts = ttk.Frame(t)
        opts.pack(anchor="w", pady=6)
        self.freeze_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        ttk.Checkbutton(opts, text="Freeze game while the overlay is open",
                        variable=self.freeze_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_freeze", bool(self.freeze_var.get()))
                        ).pack(anchor="w")
        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        ttk.Checkbutton(opts, text="Pause processing when the overlay opens",
                        variable=self.ap_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_autopause", bool(self.ap_var.get()))
                        ).pack(anchor="w")

        ttk.Label(t, text="Overlay hotkey: %s (change in config.json)"
                  % self.ctl.cfg.get("overlay_hotkey"),
                  style="Muted.TLabel").pack(anchor="w", pady=(10, 2))
        ttk.Label(t, text="Config: %s" % self.ctl.cfg.path,
                  style="Muted.TLabel").pack(anchor="w")

    # ----------------------------------------------------------- actions ---
    def _on_action_result(self, ok, msg):
        """Worker-thread callback: marshal into the Tk thread via the queue."""
        self.logq.put(("mgr", msg))

    def _daemon_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self.ctl.submit(self.ctl.daemon_start)

    def _gain_moved(self, _v):
        g = round(float(self.gain_var.get()), 3)
        # Tk fires the scale command DEFERRED (idle) even for programmatic
        # set(): drop the echo of a poll-driven sync so a stale snapshot
        # never overwrites a value the user just pushed.
        if self._suppress_scale is not None and \
                abs(g - self._suppress_scale) < 1e-9:
            self._suppress_scale = None
            return
        self._suppress_scale = None
        self.gain_lbl.config(text="%.2f" % g)
        self._pending_push = (g, time.time())
        self.root.after_idle(lambda: self.ctl.submit(self.ctl.set_gain, g))

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
        up = snap.get("daemon_pid") is not None
        if up:
            txt = "up (pid %s)" % snap["daemon_pid"]
            g, f = snap.get("daemon_gain"), snap.get("daemon_frames")
            if g is not None:
                txt += " - gain %.2f, %d frames" % (g, f or 0)
                # do not fight an in-flight user push (slider snap-back)
                pending = self._pending_push
                stale_push = pending and (time.time() - pending[1] > 2.0
                                          or abs(pending[0] - g) < 0.001)
                if stale_push:
                    self._pending_push = None
                if not self._scale_dragging and not self._pending_push:
                    self._suppress_scale = round(g, 3)
                    self.gain_scale.set(g)
                    self.gain_lbl.config(text="%.2f" % g)
            if snap.get("daemon_err"):
                txt += " [busy]"
            self.dot.config(text="O", fg=GREEN)
            self.st_daemon.config(text="daemon: " + txt, fg=GREEN)
            self.daemon_info.config(text="NRCT live control OK"
                                    if not snap.get("daemon_err") else
                                    snap["daemon_err"])
        else:
            self.dot.config(text="O", fg=RED)
            self.st_daemon.config(text="daemon: down", fg=RED)
            self.daemon_info.config(text="")

        lx = snap.get("layers_x64"), snap.get("layers_x86")
        self.layers_info.config(text="layers: x64 %s / x86 %s" % (
            "reg" if lx[0] else "MISSING", "reg" if lx[1] else "MISSING"))
        self.screen_info.config(text="overlay: %s" % (
            "UP (pid %s)" % snap["screen_pid"] if snap.get("screen_pid")
            else "off"))
        self.games_tab.render(snap)
        self._render_checklist(snap)

    def _render_checklist(self, snap):
        weights = snap.get("weights_ok")
        layers = snap.get("layers_x64") and snap.get("layers_x86")
        daemon = snap.get("daemon_pid") is not None
        games = snap.get("games") or {}
        game = any(g.get("dll") == "deployed" for g in games.values())
        states = {"weights": weights, "layers": layers, "daemon": daemon,
                  "game": game}
        for key, ok in states.items():
            base = {"weights": "Weights", "layers": "Layers",
                    "daemon": "Daemon", "game": "Game ready"}[key]
            self.steps[key].config(text=("[x] " if ok else "[ ] ") + base,
                                   fg=GREEN if ok else MUTED)
        if not weights:
            hint, color = ("Weights not found - Settings tab, Browse to "
                           "dlssnr-logical.safetensors", AMBER)
        elif not layers or not daemon:
            hint, color = "Setting up automatically...", AMBER
        elif not game:
            hint, color = ("Pick a game in the Games tab and press "
                           "'Enable DLSS 5'", AMBER)
        else:
            hint, color = ("Ready. Launch the game; %s opens the in-game "
                           "overlay." % hotkey_label(
                               self.ctl.cfg.get("overlay_hotkey")), GREEN)
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
                self.logtxt.insert("end", "[%s] %s\n" % (tag, line), tag)
            self.logtxt.see("end")
            if int(self.logtxt.index("end-1c").split(".")[0]) > 2000:
                self.logtxt.delete("1.0", "1500.0")
            self.logtxt.config(state="disabled")
        self.root.after(200, self._drain_log)

    def log(self, tag, msg):
        self.logq.put((tag, msg))

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
