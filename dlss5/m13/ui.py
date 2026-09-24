# ============================================================================
# m13.ui - DLSS 5 Manager main window (tkinter, stdlib only).
#
# Layout:
#   status bar   daemon / layer registration / screen-mode state, 1 s poll
#   daemon box   start/stop m11d, gain slider (same live NRCT push as knob)
#   notebook     Screen | DX9 game | DX12 game | Settings
#   log pane     threaded tails of m11d/m12/layer/m8blive logs
# First run: asks for the .safetensors path (proprietary weights ship with
# the USER, never with us) and remembers it in %LOCALAPPDATA%.
# ============================================================================
import os
import queue
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

from .controller import M13Controller
from .logtail import LogHub
from .overlay import GainKnob

POLL_MS = 1000

TAG_COLORS = {"m11d": "#060", "m12": "#006", "layer": "#a60",
              "m8blive": "#666", "mgr": "#000"}


class ManagerUI:
    def __init__(self):
        self.ctl = M13Controller()
        self.root = tk.Tk()
        self._setting_scale = False   # guard: programmatic scale sets
        self.root.title("DLSS 5 Manager (M13)")
        self.root.geometry("860x640")
        self.logq = queue.Queue()
        self.hub = LogHub(self._enqueue_log)
        self._build()
        self.hub.start()
        self.root.after(200, self._drain_log)
        self.root.after(POLL_MS, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        if self.ctl.cfg.first_run:
            self.root.after(300, self._first_run_weights)

    # ------------------------------------------------------------- build ---
    def _build(self):
        bar = tk.Frame(self.root, relief="groove", bd=1)
        bar.pack(fill="x", side="top")
        self.st_daemon = tk.Label(bar, text="m11d: ?")
        self.st_daemon.pack(side="left", padx=6)
        self.st_layers = tk.Label(bar, text="layers: ?")
        self.st_layers.pack(side="left", padx=6)
        self.st_screen = tk.Label(bar, text="screen: ?")
        self.st_screen.pack(side="left", padx=6)
        self.st_mode = tk.Label(bar, text="", fg="#06c")
        self.st_mode.pack(side="right", padx=6)

        daemon = tk.LabelFrame(self.root, text="daemon (m11d - the 71-block chain)")
        daemon.pack(fill="x", padx=6, pady=4)
        tk.Button(daemon, text="Start", command=self._daemon_start).pack(side="left", padx=4, pady=4)
        tk.Button(daemon, text="Stop", command=self._daemon_stop).pack(side="left")
        tk.Label(daemon, text="gain:").pack(side="left", padx=(14, 2))
        self.gain_var = tk.DoubleVar(value=self.ctl.cfg.get("gain"))
        self.gain_scale = tk.Scale(daemon, from_=0.0, to=2.0, resolution=0.05,
                                   orient="horizontal", length=220,
                                   variable=self.gain_var,
                                   command=self._gain_moved)
        self.gain_scale.pack(side="left")
        self.daemon_info = tk.Label(daemon, text="", fg="#666")
        self.daemon_info.pack(side="left", padx=10)
        tk.Button(daemon, text="Gain knob (hotkey)",
                  command=self._knob_toggle).pack(side="right", padx=4)

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=6)
        self.nb = nb
        self._tab_screen(nb)
        self._tab_dx9(nb)
        self._tab_dx12(nb)
        self._tab_settings(nb)

        self.logtxt = ScrolledText(self.root, height=12, state="disabled",
                                   font=("Consolas", 9))
        self.logtxt.pack(fill="both", expand=False, padx=6, pady=4)
        for tag, color in TAG_COLORS.items():
            self.logtxt.tag_config(tag, foreground=color)

        self.knob = GainKnob(self.root, on_gain=self.ctl.set_gain,
                             on_log=lambda m: self.log("mgr", m),
                             hotkey_spec=self.ctl.cfg.get(
                                 "overlay_hotkey"),
                             initial=self.ctl.cfg.get("gain"))

    def _tab_screen(self, nb):
        t = tk.Frame(nb, padx=8, pady=8)
        nb.add(t, text="Screen mode")
        tk.Label(t, text="Fullscreen overlay enhancing the whole desktop "
                         "(watch it through the Moonlight stream).",
                 wraplength=560, justify="left").pack(anchor="w")
        row = tk.Frame(t); row.pack(anchor="w", pady=6)
        tk.Button(row, text="Start overlay",
                  command=self._screen_start).pack(side="left")
        tk.Button(row, text="Stop overlay",
                  command=self._screen_stop).pack(side="left", padx=6)
        tk.Label(row, text="hotkeys while running: CTRL+ALT+X hide/show, "
                           "CTRL+ALT+Q quit").pack(side="left", padx=12)
        self.screen_info = tk.Label(t, text="overlay: ?")
        self.screen_info.pack(anchor="w")

    def _game_tab(self, nb, title, key, deploy_label, launch_fn, status_fn,
                  deploy_fn, undeploy_fn, pause_fn, resume_fn, paused_fn,
                  extra_note=""):
        t = tk.Frame(nb, padx=8, pady=8)
        nb.add(t, text=title)
        if extra_note:
            tk.Label(t, text=extra_note, wraplength=560, justify="left",
                     fg="#a60").pack(anchor="w")
        row = tk.Frame(t); row.pack(anchor="w", pady=4, fill="x")
        tk.Label(row, text="game exe:").pack(side="left")
        var = tk.StringVar(value=self.ctl.cfg.get(key) or "")
        ent = tk.Entry(row, textvariable=var, width=64)
        ent.pack(side="left", padx=4)
        def browse():
            p = filedialog.askopenfilename(title="game exe",
                                           filetypes=[("exe", "*.exe")])
            if p:
                var.set(p)
                self.ctl.cfg.set(key, p)
        tk.Button(row, text="Browse...", command=browse).pack(side="left")

        row2 = tk.Frame(t); row2.pack(anchor="w", pady=4)
        tk.Button(row2, text=deploy_label,
                  command=lambda: self._act(deploy_fn, var.get())).pack(side="left")
        tk.Button(row2, text="Undeploy",
                  command=lambda: self._act(undeploy_fn, var.get())).pack(side="left", padx=4)
        tk.Button(row2, text="Launch game",
                  command=lambda: self._act(launch_fn, var.get())).pack(side="left", padx=4)
        tk.Button(row2, text="Pause (full fps)",
                  command=lambda: self._act2(pause_fn)).pack(side="left", padx=4)
        tk.Button(row2, text="Resume",
                  command=lambda: self._act2(resume_fn)).pack(side="left")

        info = tk.Label(t, text="status: ?", justify="left")
        info.pack(anchor="w", pady=4)
        return {"tab": t, "var": var, "info": info, "status_fn": status_fn,
                "paused_fn": paused_fn}

    def _tab_dx9(self, nb):
        self.dx9 = self._game_tab(
            nb, "DX9 game", "dx9_game", "Deploy d3d9.dll (DXVK x32)",
            self.ctl.dx9_launch, self.ctl.dx9_status, self.ctl.dx9_deploy,
            self.ctl.dx9_undeploy, self.ctl.dx9_pause, self.ctl.dx9_resume,
            gamelaunch_paused_dx9,
            "DX9: ONLY d3d9.dll is deployed (dxgi.dll from DXVK crashes GTA IV "
            "in system d3d11). The m11 Vulkan layer must be registered "
            "(Settings). Pause/resume is a file channel, no key injection.")

    def _tab_dx12(self, nb):
        self.dx12 = self._game_tab(
            nb, "DX12 game", "dx12_game", "Deploy dxgi.dll proxy",
            self.ctl.dx12_launch, self.ctl.dx12_status, self.ctl.dx12_deploy,
            self.ctl.dx12_undeploy, self.ctl.dx12_pause, self.ctl.dx12_resume,
            gamelaunch_paused_dx12,
            "DX12: m12-dxgi proxy. The game must be RESTARTED after deploy; "
            "anti-cheat may block it (BattleEye blocked it in GTA5 - owner "
            "disabled BE). GTAO is off-limits with the proxy deployed.")

    def _tab_settings(self, nb):
        t = tk.Frame(nb, padx=8, pady=8)
        nb.add(t, text="Settings")
        row = tk.Frame(t); row.pack(anchor="w", pady=4, fill="x")
        tk.Label(row, text="weights (.safetensors):").pack(side="left")
        self.weights_var = tk.StringVar(value=self.ctl.cfg.get("weights_path") or "")
        tk.Entry(row, textvariable=self.weights_var, width=58).pack(side="left", padx=4)
        def browse_weights():
            p = filedialog.askopenfilename(
                title="dlssnr-logical.safetensors",
                filetypes=[("safetensors", "*.safetensors"), ("all", "*.*")])
            if p:
                self.weights_var.set(p)
                self.ctl.cfg.set("weights_path", p)
                self.log("mgr", "weights path set: %s" % p)
        tk.Button(row, text="Browse...", command=browse_weights).pack(side="left")
        tk.Button(row, text="Save", command=lambda: self.ctl.cfg.set(
            "weights_path", self.weights_var.get())).pack(side="left", padx=4)

        row2 = tk.Frame(t); row2.pack(anchor="w", pady=4)
        tk.Button(row2, text="Register Vulkan layers (HKCU)",
                  command=lambda: self._act(self.ctl.layers_register)).pack(side="left")
        tk.Button(row2, text="Unregister",
                  command=lambda: self._act(self.ctl.layers_unregister)).pack(side="left", padx=4)
        self.layers_info = tk.Label(row2, text="layers: ?")
        self.layers_info.pack(side="left", padx=10)

        tk.Label(t, text="Overlay hotkey: %s (change in config.json)" %
                         self.ctl.cfg.get("overlay_hotkey"),
                 fg="#666").pack(anchor="w", pady=4)
        tk.Label(t, text="Config: %s" % self.ctl.cfg.path, fg="#666").pack(anchor="w")

    # ----------------------------------------------------------- actions ---
    def _act(self, fn, arg=None):
        try:
            ok, msg = fn(arg) if arg is not None else fn()
        except Exception as e:
            ok, msg = False, "error: %s" % e
        self.log("mgr", "%s" % msg)
        self._poll()

    def _act2(self, fn):
        self._act(fn)

    def _daemon_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self._act(self.ctl.daemon_start)

    def _daemon_stop(self):
        self._act(self.ctl.daemon_stop)

    def _screen_start(self):
        self.ctl.cfg.set("gain", round(float(self.gain_var.get()), 3))
        self._act(self.ctl.screen_start)

    def _screen_stop(self):
        self._act(self.ctl.screen_stop)

    def _gain_moved(self, _v):
        if self._setting_scale:
            return
        # slider release = push live (same NRCT channel as the knob)
        self.root.after_idle(self._gain_push)

    def _gain_push(self):
        g = round(float(self.gain_var.get()), 3)
        if abs(g - self.ctl.cfg.get("gain")) < 1e-9:
            return
        ok, msg = self.ctl.set_gain(g)
        self.log("mgr", msg)

    def _knob_toggle(self):
        self.knob.toggle()

    # -------------------------------------------------------------- poll ---
    def _poll(self):
        try:
            st = self.ctl.daemon_status()
            if st["running"]:
                txt = "m11d: UP (pid %s)" % self.ctl.daemon.pid
                if st["gain"] is not None:
                    txt += " gain %.2f, %d frames" % (st["gain"], st["frames"])
                    self._setting_scale = True
                    try:
                        self.gain_scale.set(st["gain"])
                    finally:
                        self._setting_scale = False
                if st["error"]:
                    txt += " [%s]" % st["error"]
                self.st_daemon.config(fg="#060", text=txt)
                self.knob.set_status("daemon gain %.2f, %d frames"
                                     % (st["gain"] or 0, st["frames"] or 0))
            else:
                self.st_daemon.config(fg="#a00", text="m11d: DOWN")
                self.knob.set_status("daemon down")
            ls = self.ctl.layers_status()
            self.st_layers.config(
                fg="#060" if all(ls.values()) else "#a60",
                text="layers: x64 %s / x86 %s"
                     % ("reg" if ls["x64"] else "MISSING",
                        "reg" if ls["x86"] else "MISSING"))
            self.layers_info.config(text=self.st_layers.cget("text"))
            self.st_screen.config(
                fg="#060" if self.ctl.screen.running else "#666",
                text="screen: %s" % ("UP" if self.ctl.screen.running else "off"))
            self._game_poll(self.dx9, "DX9")
            self._game_poll(self.dx12, "DX12")
        except Exception as e:
            self.log("mgr", "poll error: %s" % e)
        self.root.after(POLL_MS, self._poll)

    def _game_poll(self, tab, name):
        exe = tab["var"].get().strip()
        if not exe:
            tab["info"].config(text="status: no game exe picked")
            return
        try:
            st = tab["status_fn"](exe)
            parts = ["dll: %s" % st.get("dll", "?")]
            for g in st.get("guards", []):
                parts.append("WARNING: %s" % g)
            parts.append("processing: %s" % ("PAUSED" if tab["paused_fn"]()
                                             else "on"))
            tab["info"].config(text="status: " + " | ".join(parts))
        except Exception as e:
            tab["info"].config(text="status: probe failed: %s" % e)

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
            # keep the pane bounded (check once per drain, not per line)
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
        self._knob_toggle()     # show the knob so the hotkey is discoverable
        self.knob.hide()

    def _on_close(self):
        self.hub.stop()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def gamelaunch_paused_dx9():
    from . import gamelaunch
    return gamelaunch.dx9_paused()


def gamelaunch_paused_dx12():
    from . import gamelaunch
    return gamelaunch.dx12_paused()


def main():
    ManagerUI().run()
