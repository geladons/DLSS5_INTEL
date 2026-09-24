# ============================================================================
# m13.ui_games - the Games tab: drive scanner results + one-click enable /
# launch / disable. The demo must be hands-free: scanning starts by itself,
# "Launch" auto-deploys and auto-starts the daemon when needed.
#
# All heavy work (drive scan, deploy, launch) runs off the Tk thread; this
# class only renders and queues.
# ============================================================================
import queue
import threading
import tkinter as tk
from tkinter import filedialog, ttk

MODE_NAMES = {"dx9": "DX9", "dx11": "DX11", "dx12": "DX12",
              "vulkan": "Vulkan"}


class GamesTab:
    def __init__(self, ui, nb):
        self.ui = ui                # ManagerUI: ctl, log(), colors via ui
        self.ctl = ui.ctl
        self.scanq = queue.Queue()
        self.scanned = []
        self._build(nb)

    def _build(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text="  Games  ")
        top = ttk.Frame(t)
        top.pack(fill="x")
        ttk.Button(top, text="Rescan drives", style="Accent.TButton",
                   command=self.scan_start).pack(side="left")
        ttk.Button(top, text="Add exe manually...",
                   command=self._add_manual).pack(side="left", padx=6)
        self.scan_info = ttk.Label(top, text="scanning...",
                                   style="Muted.TLabel")
        self.scan_info.pack(side="left", padx=10)

        cols = ("name", "mode", "state", "path")
        self.tree = ttk.Treeview(t, columns=cols, show="headings", height=10)
        for cid, label, w in (("name", "Game", 200), ("mode", "Mode", 70),
                              ("state", "Status", 170), ("path", "Path", 420)):
            self.tree.heading(cid, text=label)
            self.tree.column(cid, width=w, anchor="w")
        self.tree.tag_configure("running", foreground="#3fb950")
        self.tree.tag_configure("enabled", foreground="#4f8cff")
        self.tree.tag_configure("foreign", foreground="#f85149")
        self.tree.tag_configure("warn", foreground="#d29922")
        self.tree.tag_configure("plain", foreground="#8b919e")
        self.tree.pack(fill="both", expand=True, pady=6)
        self.tree.bind("<<TreeviewSelect>>", lambda _e: self._sel_changed())

        act = ttk.Frame(t)
        act.pack(fill="x", pady=(2, 0))
        ttk.Label(act, text="mode:").pack(side="left")
        self.mode_var = tk.StringVar(value="auto")
        self.mode_box = ttk.Combobox(act, textvariable=self.mode_var, width=7,
                                     state="readonly",
                                     values=("auto", "DX9", "DX11", "DX12",
                                             "Vulkan"))
        self.mode_box.pack(side="left", padx=(4, 10))
        ttk.Button(act, text="Enable DLSS 5", style="Accent.TButton",
                   command=self._enable).pack(side="left")
        ttk.Button(act, text="Launch",
                   command=self._launch).pack(side="left", padx=6)
        ttk.Button(act, text="Pause / Resume",
                   command=lambda: self.ctl.submit(
                       self.ctl.processing_toggle)).pack(side="left")
        ttk.Button(act, text="Disable",
                   command=self._disable).pack(side="left", padx=6)
        ttk.Button(act, text="Remove", style="Danger.TButton",
                   command=self._remove).pack(side="left")
        ttk.Label(t, text="Anti-cheat warning: do not enable in online/"
                          "protected titles (PUBG, CS2, GTA Online) - the "
                          "DLL injection can be read as a cheat.",
                  style="Muted.TLabel", foreground="#d29922",
                  wraplength=860).pack(anchor="w", pady=(6, 0))

    # ---------------------------------------------------------- selection --
    def selected_exe(self):
        sel = self.tree.selection()
        if not sel:
            return None
        tags = self.tree.item(sel[0], "tags")
        return tags[-1] if tags else None

    def _sel_changed(self):
        exe = self.selected_exe()
        meta = (self.ctl.cfg.get("games") or {}).get(exe or "")
        if meta:
            self.mode_var.set(MODE_NAMES.get(meta["mode"], "auto"))

    def _chosen_mode(self):
        v = self.mode_var.get().lower()
        return None if v == "auto" else v

    # ------------------------------------------------------------ actions --
    def _add_manual(self):
        p = filedialog.askopenfilename(title="game exe",
                                       filetypes=[("exe", "*.exe")])
        if p:
            self.ctl.submit(self.ctl.add_game, p, None, None, None)

    def _enable(self):
        exe = self.selected_exe()
        if not exe:
            self.ui.log("mgr", "pick a game in the list first")
            return
        self.ctl.submit(self._enable_worker, exe, self._chosen_mode())

    def _enable_worker(self, exe, mode):
        ok, msg = self.ctl.add_game(exe, mode)
        if not ok:
            return False, msg
        return self.ctl.game_deploy(exe)

    def _launch(self):
        exe = self.selected_exe()
        if not exe:
            self.ui.log("mgr", "pick a game in the list first")
            return
        if exe not in (self.ctl.cfg.get("games") or {}):
            self.ctl.submit(self._enable_worker, exe, self._chosen_mode())
        self.ctl.submit(self.ctl.game_launch, exe)

    def _disable(self):
        exe = self.selected_exe()
        if exe:
            self.ctl.submit(self.ctl.game_undeploy, exe)

    def _remove(self):
        exe = self.selected_exe()
        if exe:
            self.ctl.submit(self.ctl.remove_game, exe)

    # --------------------------------------------------------------- scan --
    def scan_start(self):
        self.scan_info.config(text="scanning drives...")
        threading.Thread(target=self._scan_worker, daemon=True,
                         name="m13-scan").start()

    def _scan_worker(self):
        from . import gamescan
        try:
            games = gamescan.scan()
        except Exception as e:
            self.scanq.put(("error", e))
            return
        self.scanq.put(("ok", games))

    def scan_drain(self):
        """Called from the UI poll; applies finished scan results."""
        try:
            kind, payload = self.scanq.get_nowait()
        except queue.Empty:
            return
        if kind == "error":
            self.scan_info.config(text="scan failed: %s" % payload)
            return
        self.scanned = payload
        self.scan_info.config(text="%d game candidates found"
                              % len(self.scanned))
        self.ui.log("mgr", "game scan: %d candidates" % len(self.scanned))
        self.tree_fill()

    def tree_fill(self):
        self.tree.delete(*self.tree.get_children())
        known = self.ctl.cfg.get("games") or {}
        rows = []
        for g in self.scanned:
            rows.append((g.exe, g.name + (" *" if g.likely else ""),
                         (g.mode or "?").upper() + ("/" + g.arch
                                                    if g.arch else ""),
                         g.anticheat))
        for exe, meta in known.items():
            if all(r[0] != exe for r in rows):
                rows.append((exe, meta["name"] + " (saved)",
                             meta["mode"].upper(), False))
        for exe, name, mode, ac in rows:
            self.tree.insert("", "end", values=(name, mode, "...", exe),
                             tags=("warn" if ac else "plain", exe))

    # ------------------------------------------------------------- render --
    def render(self, snap):
        games = snap.get("games") or {}
        for item in self.tree.get_children():
            tags = self.tree.item(item, "tags")
            exe = tags[-1] if tags else ""
            vals = list(self.tree.item(item, "values"))
            meta = games.get(exe)
            warn = "warn" in tags
            if meta:
                state = meta["dll"] or "?"
                if meta["running"]:
                    state += " | RUNNING"
                    if meta["paused"]:
                        state += " (paused)"
                    tag = "running"
                elif meta["dll"] == "deployed":
                    tag = "enabled"
                elif meta["dll"] in ("foreign", "partial"):
                    tag = "foreign"
                else:
                    tag = "warn" if warn else "plain"
            else:
                state = "not added"
                tag = "warn" if warn else "plain"
            if warn and tag in ("plain", "enabled"):
                state += " | ANTI-CHEAT RISK"
            vals[2] = state
            self.tree.item(item, values=vals, tags=(tag, exe))
