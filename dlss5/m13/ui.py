# ============================================================================
# m13.ui - DLSS 5 Manager main window (tkinter, stdlib only, dark theme).
#
# Layout:
#   header       title + daemon state dot + start/stop + gain + overlay btn
#   checklist    4 setup steps (weights / layers / game / daemon) - what to
#                do next is always visible, nothing is "hidden in tabs"
#   notebook     Screen | DX9 game | DX12 game | Settings
#   log pane     threaded tails of m11d/m12/layer/m8blive logs
#
# RESPONSIVENESS RULE: this file NEVER calls a blocking probe. All state
# comes from ctl.snapshot() (StateMonitor thread); every action goes through
# ctl.submit() (ActionWorker thread). The Tk thread only renders. This is
# what keeps the window alive while m11d is busy processing frames.
# ============================================================================
import os
import queue
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

from .controller import M13Controller
from .logtail import LogHub
from .overlay import ControlOverlay, hotkey_label

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
        self._setting_scale = False
        self.root.title("DLSS 5 Manager")
        self.root.geometry("920x700")
        self.root.minsize(820, 600)
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
        if self.ctl.cfg.first_run:
            self.root.after(300, self._first_run_weights)

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
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=FG)
        s.configure("Panel.TLabel", background=PANEL, foreground=FG)
        s.configure("Muted.TLabel", background=BG, foreground=MUTED)
        s.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED)
        s.configure("Title.TLabel", background=BG, foreground=FG,
                    font=("Segoe UI", 13, "bold"))
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

    # ------------------------------------------------------------- build ---
    def _build(self):
        self._build_header()
        self._build_checklist()

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=10, pady=(4, 0))
        self.nb = nb
        self._tab_screen(nb)
        self._tab_dx9(nb)
        self._tab_dx12(nb)
        self._tab_settings(nb)

        self.logtxt = ScrolledText(self.root, height=10, state="disabled",
                                   font=("Consolas", 9), bg="#101216",
                                   fg=FG, insertbackground=FG,
                                   relief="flat", bd=6)
        self.logtxt.pack(fill="both", expand=False, padx=10, pady=6)
        for tag, color in TAG_COLORS.items():
            self.logtxt.tag_config(tag, foreground=color)

        self.overlay = ControlOverlay(
            self.root, self.ctl, on_log=lambda m: self.log("mgr", m))
        self.knob = self.overlay          # back-compat alias

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
            command=self.overlay_toggle).pack(side="right")

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
        self._suppress_scale = None   # value set programmatically: ignore the
        self._scale_dragging = False  # deferred command Tk fires after set()
        self.gain_scale = tk.Scale(
            inner, from_=0.0, to=2.0, resolution=0.05, orient="horizontal",
            length=200, variable=self.gain_var, command=self._gain_moved,
            bg=PANEL, fg=FG, troughcolor=PANEL2, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.gain_scale.pack(side="left")
        self.gain_scale.bind("<ButtonPress-1>",
                             lambda _e: setattr(self, "_scale_dragging", True))
        self.gain_scale.bind("<ButtonRelease-1>",
                             lambda _e: setattr(self, "_scale_dragging", False))
        self.gain_lbl = tk.Label(inner, text="%.2f" % self.gain_var.get(),
                                 bg=PANEL, fg=ACCENT, font=("Consolas", 10,
                                                            "bold"), width=5)
        self.gain_lbl.pack(side="left")
        self.daemon_info = tk.Label(inner, text="", bg=PANEL, fg=MUTED,
                                    font=("Segoe UI", 8))
        self.daemon_info.pack(side="right")

    def _build_checklist(self):
        box = tk.Frame(self.root, bg=PANEL)
        box.pack(fill="x", padx=10, pady=4)
        row = tk.Frame(box, bg=PANEL)
        row.pack(fill="x", padx=8, pady=6)
        tk.Label(row, text="Setup:", bg=PANEL, fg=MUTED,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.steps = {}
        for key, label in (("weights", "1. Weights"),
                           ("layers", "2. Vulkan layers"),
                           ("game", "3. Game picked"),
                           ("daemon", "4. Daemon up")):
            lbl = tk.Label(row, text=label, bg=PANEL, fg=MUTED,
                           font=("Segoe UI", 9), padx=10)
            lbl.pack(side="left")
            self.steps[key] = lbl
        self.hint = tk.Label(box, text="", bg=PANEL, fg=AMBER,
                             font=("Segoe UI", 9), anchor="w")
        self.hint.pack(fill="x", padx=8, pady=(0, 6))

    # --------------------------------------------------------------- tabs --
    def _tab_screen(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text="  Screen mode  ")
        ttk.Label(t, text="Fullscreen overlay enhancing the whole desktop. "
                          "The effect is visible in the Moonlight stream.",
                  style="Muted.TLabel", wraplength=700,
                  justify="left").pack(anchor="w")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=8)
        ttk.Button(row, text="Start overlay", style="Accent.TButton",
                   command=self._screen_start).pack(side="left")
        ttk.Button(row, text="Stop overlay", style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.screen_stop)).pack(side="left", padx=6)
        ttk.Label(row, text="hotkeys while running: CTRL+ALT+X hide/show, "
                            "CTRL+ALT+Q quit",
                  style="Muted.TLabel").pack(side="left", padx=14)
        self.screen_info = ttk.Label(t, text="overlay: ?",
                                     style="Muted.TLabel")
        self.screen_info.pack(anchor="w")

    def _game_tab(self, nb, title, key, deploy_label, launch_fn,
                  deploy_fn, undeploy_fn, mode, extra_note=""):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text=title)
        if extra_note:
            ttk.Label(t, text=extra_note, style="Muted.TLabel",
                      wraplength=700, justify="left",
                      foreground=AMBER).pack(anchor="w", pady=(0, 6))
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=4, fill="x")
        ttk.Label(row, text="game exe:").pack(side="left")
        var = tk.StringVar(value=self.ctl.cfg.get(key) or "")
        ent = ttk.Entry(row, textvariable=var, width=70)
        ent.pack(side="left", padx=6)
        ent.bind("<FocusOut>", lambda _e: self.ctl.cfg.set(key, var.get()))

        def browse():
            p = filedialog.askopenfilename(title="game exe",
                                           filetypes=[("exe", "*.exe")])
            if p:
                var.set(p)
                self.ctl.cfg.set(key, p)
        ttk.Button(row, text="Browse...", command=browse).pack(side="left")

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=6)
        ttk.Button(row2, text=deploy_label,
                   command=lambda: self.ctl.submit(
                       deploy_fn, var.get())).pack(side="left")
        ttk.Button(row2, text="Undeploy",
                   command=lambda: self.ctl.submit(
                       undeploy_fn, var.get())).pack(side="left", padx=6)
        ttk.Button(row2, text="Launch game", style="Accent.TButton",
                   command=lambda: self.ctl.submit(
                       launch_fn, var.get())).pack(side="left", padx=6)
        ttk.Button(row2, text="Pause / Resume",
                   command=self._pause_mode(mode)).pack(side="left", padx=6)

        info = ttk.Label(t, text="status: ?", style="Muted.TLabel",
                         justify="left")
        info.pack(anchor="w", pady=4)
        return {"tab": t, "var": var, "info": info, "mode": mode}

    def _pause_mode(self, mode):
        def fn():
            from . import gamelaunch
            if mode == "dx9":
                self.ctl.submit(gamelaunch.dx9_resume
                                if gamelaunch.dx9_paused()
                                else gamelaunch.dx9_pause)
            else:
                self.ctl.submit(gamelaunch.dx12_resume
                                if gamelaunch.dx12_paused()
                                else gamelaunch.dx12_pause)
        return fn

    def _tab_dx9(self, nb):
        self.dx9 = self._game_tab(
            nb, "  DX9 game  ", "dx9_game", "Deploy d3d9.dll (DXVK x32)",
            self.ctl.dx9_launch, self.ctl.dx9_deploy, self.ctl.dx9_undeploy,
            "dx9",
            "DX9: ONLY d3d9.dll is deployed (dxgi.dll from DXVK crashes GTA "
            "IV in system d3d11). The m11 Vulkan layer must be registered "
            "(step 2). Pause/resume is a file channel, no key injection.")

    def _tab_dx12(self, nb):
        self.dx12 = self._game_tab(
            nb, "  DX12 game  ", "dx12_game", "Deploy dxgi.dll proxy",
            self.ctl.dx12_launch, self.ctl.dx12_deploy,
            self.ctl.dx12_undeploy, "dx12",
            "DX12: m12-dxgi proxy. The game must be RESTARTED after deploy; "
            "anti-cheat may block it (BattleEye blocked it in GTA5). GTAO "
            "is off-limits with the proxy deployed.")

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
                   style="Accent.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_register)).pack(side="left")
        ttk.Button(row2, text="Unregister",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_unregister)).pack(side="left", padx=6)
        self.layers_info = ttk.Label(row2, text="layers: ?",
                                     style="Muted.TLabel")
        self.layers_info.pack(side="left", padx=10)

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

    def _screen_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self.ctl.submit(self.ctl.screen_start)

    def overlay_toggle(self):
        self.overlay.toggle()

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
        self.root.after_idle(lambda: self.ctl.submit(self.ctl.set_gain, g))

    # -------------------------------------------------------------- poll ---
    def _poll(self):
        """Render the monitor snapshot. Zero I/O on this thread."""
        try:
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
                if not self._scale_dragging:
                    self._suppress_scale = round(g, 3)
                    self.gain_scale.set(g)
                    self.gain_lbl.config(text="%.2f" % g)
            if snap.get("daemon_err"):
                txt += " [busy]"
            self.dot.config(text="O", fg=GREEN)
            self.st_daemon.config(text="daemon: " + txt, fg=GREEN)
            self.daemon_info.config(text="NRCT live control channel OK"
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

        self._render_game(self.dx9, snap, "dx9")
        self._render_game(self.dx12, snap, "dx12")
        self._render_checklist(snap)

    def _render_game(self, tab, snap, mode):
        exe = tab["var"].get().strip()
        if not exe:
            tab["info"].config(text="status: no game exe picked")
            return
        parts = ["dll: %s" % (snap.get(mode + "_dll") or "?")]
        if snap.get(mode + "_running"):
            parts.append("game running")
        parts.append("processing: %s" % ("PAUSED" if snap.get(mode + "_paused")
                                         else "on"))
        for g in snap.get(mode + "_guards", []):
            parts.append("WARNING: %s" % g)
        tab["info"].config(text="status: " + " | ".join(parts))

    def _render_checklist(self, snap):
        weights = snap.get("weights_ok")
        layers = snap.get("layers_x64") and snap.get("layers_x86")
        game = bool(snap.get("dx9_exe") or snap.get("dx12_exe"))
        daemon = snap.get("daemon_pid") is not None
        states = {"weights": weights, "layers": layers, "game": game,
                  "daemon": daemon}
        for key, ok in states.items():
            base = {"weights": "1. Weights", "layers": "2. Vulkan layers",
                    "game": "3. Game picked", "daemon": "4. Daemon up"}[key]
            self.steps[key].config(
                text=("[x] " if ok else "[ ] ") + base,
                fg=GREEN if ok else MUTED)
        if not weights:
            hint = "Next: Settings tab - point at dlssnr-logical.safetensors"
        elif not layers:
            hint = "Next: Settings tab - Register Vulkan layers (one click)"
        elif not game:
            hint = "Next: DX9/DX12 tab - pick the game exe, Deploy, Launch"
        elif not daemon:
            hint = "Next: press Start daemon above"
        else:
            hint = "Ready. In game: %s opens the live overlay." % \
                hotkey_label(self.ctl.cfg.get("overlay_hotkey"))
            self.hint.config(fg=GREEN)
            self.hint.config(text=hint)
            return
        self.hint.config(fg=AMBER)
        self.hint.config(text=hint)

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
    def _first_run_weights(self):
        self.log("mgr", "first run: pick the model weights (.safetensors)")
        self.nb.select(3)

    def _on_close(self):
        self.ctl.shutdown()
        self.hub.stop()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def main():
    ManagerUI().run()
