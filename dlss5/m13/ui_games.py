# ============================================================================
# m13.ui_games - the Games tab: found games as ICON CARDS (owner feedback:
# "a list of 180 exe files is unreadable - show the actual games as icons").
# One card per real game (gamescan groups candidates; only API-positive
# install roots become cards). A card shows the exe icon, the game name,
# the detected graphics API, the launch wrapper (pirate/GOG stubs) and the
# live status (enabled / running / anti-cheat risk).
#
# All heavy work (drive scan, deploy, launch) runs off the Tk thread; this
# class only renders and queues.
# ============================================================================
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, ttk

from .i18n import tr
from .icons import IconCache

MODE_NAMES = {"dx9": "DX9", "dx11": "DX11", "dx12": "DX12",
              "vulkan": "Vulkan"}
MODE_COLORS = {"dx9": "#d29922", "dx11": "#58a6ff", "dx12": "#9a6bff",
               "vulkan": "#3fb950", None: "#8a91a5"}

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

CARD_W = 150
CARD_H = 172
CARD_COLS = 4


class GameCard:
    """One clickable icon card in the grid."""

    def __init__(self, tab, parent, exe, name, mode, arch, source,
                 launch_exe=None, anticheat=False, saved=False):
        self.tab = tab
        self.exe = exe
        self.anticheat = anticheat
        self.frame = tk.Frame(parent, bg=PANEL, width=CARD_W, height=CARD_H,
                              highlightthickness=1,
                              highlightbackground=PANEL2,
                              highlightcolor=ACCENT, cursor="hand2")
        self.frame.pack_propagate(False)
        self.icon_lbl = tk.Label(self.frame, bg=PANEL)
        self.icon_lbl.pack(pady=(10, 4))
        img = tab.icons.get(self.frame, exe)
        self.icon_lbl.config(image=img)
        self._img = img     # keep a reference
        self.name_lbl = tk.Label(self.frame, text=name, bg=PANEL, fg=FG,
                                 font=("Segoe UI", 9, "bold"),
                                 wraplength=CARD_W - 14, justify="center")
        self.name_lbl.pack()
        mid = tk.Frame(self.frame, bg=PANEL)
        mid.pack(pady=(2, 0))
        color = MODE_COLORS.get(mode, MODE_COLORS[None])
        tk.Label(mid, text=MODE_NAMES.get(mode, "?"), bg=PANEL, fg=color,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        if arch:
            tk.Label(mid, text=" " + arch, bg=PANEL, fg=MUTED,
                     font=("Segoe UI", 8)).pack(side="left")
        if anticheat:
            tk.Label(mid, text=" ⚠", bg=PANEL, fg=AMBER,
                     font=("Segoe UI", 9, "bold")).pack(side="left")
        sub = []
        if launch_exe:
            sub.append(tr("via", os.path.basename(launch_exe)))
        if saved:
            sub.append(tr("saved_mark"))
        self.sub_lbl = tk.Label(self.frame, text=" · ".join(sub) or source,
                                bg=PANEL, fg=MUTED, font=("Segoe UI", 7),
                                wraplength=CARD_W - 12, justify="center")
        self.sub_lbl.pack()
        self.state_lbl = tk.Label(self.frame, text=tr("state_not_added"),
                                  bg=PANEL, fg=MUTED, font=("Segoe UI", 8))
        self.state_lbl.pack(pady=(3, 0))
        for w in (self.frame, self.icon_lbl, self.name_lbl, self.sub_lbl,
                  self.state_lbl, mid):
            w.bind("<Button-1>", self._click)
            w.bind("<Double-Button-1>", self._dbl)
        for w in mid.winfo_children():
            w.bind("<Button-1>", self._click)
            w.bind("<Double-Button-1>", self._dbl)

    def _click(self, _e):
        self.tab.select(self.exe)

    def _dbl(self, _e):
        self.tab.select(self.exe)
        self.tab._launch()

    def set_selected(self, on):
        self.frame.config(highlightbackground=ACCENT if on else PANEL2,
                          bg=PANEL2 if on else PANEL)
        for w in (self.icon_lbl, self.name_lbl, self.sub_lbl,
                  self.state_lbl):
            w.config(bg=PANEL2 if on else PANEL)

    def set_state(self, text, color):
        self.state_lbl.config(text=text, fg=color)


class GamesTab:
    def __init__(self, ui, nb):
        self.ui = ui                # ManagerUI: ctl, log(), colors via ui
        self.ctl = ui.ctl
        self.scanq = queue.Queue()
        self.scanned = []
        self.cards = {}             # exe -> GameCard
        self._selected = None
        self.icons = IconCache(size=48)
        self._build(nb)

    def _build(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text=tr("tab_games"))
        self.tab_frame = t
        top = ttk.Frame(t)
        top.pack(fill="x")
        ttk.Button(top, text=tr("rescan"), style="Accent.TButton",
                   command=self.scan_start).pack(side="left")
        ttk.Button(top, text=tr("add_manual"),
                   command=self._add_manual).pack(side="left", padx=6)
        self.scan_info = ttk.Label(top, text=tr("scanning"),
                                   style="Muted.TLabel")
        self.scan_info.pack(side="left", padx=10)

        # scrollable card grid
        wrap = tk.Frame(t, bg=BG)
        wrap.pack(fill="both", expand=True, pady=6)
        self.canvas = tk.Canvas(wrap, bg=BG, highlightthickness=0, height=380)
        vsb = ttk.Scrollbar(wrap, orient="vertical",
                            command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.grid_frame = tk.Frame(self.canvas, bg=BG)
        self.canvas.create_window((0, 0), window=self.grid_frame, anchor="nw")
        self.grid_frame.bind("<Configure>", lambda _e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind_all("<MouseWheel>", self._wheel)

        act = ttk.Frame(t)
        act.pack(fill="x", pady=(2, 0))
        ttk.Label(act, text=tr("mode_label")).pack(side="left")
        self.mode_var = tk.StringVar(value=tr("mode_auto"))
        self.mode_box = ttk.Combobox(
            act, textvariable=self.mode_var, width=7, state="readonly",
            values=(tr("mode_auto"), "DX9", "DX11", "DX12", "Vulkan"))
        self.mode_box.pack(side="left", padx=(4, 10))
        ttk.Button(act, text=tr("enable"), style="Accent.TButton",
                   command=self._enable).pack(side="left")
        ttk.Button(act, text=tr("play"),
                   command=self._launch).pack(side="left", padx=6)
        ttk.Button(act, text=tr("pause_resume"),
                   command=lambda: self.ctl.submit(
                       self.ctl.processing_toggle)).pack(side="left")
        ttk.Button(act, text=tr("disable"),
                   command=self._disable).pack(side="left", padx=6)
        ttk.Button(act, text=tr("remove"), style="Danger.TButton",
                   command=self._remove).pack(side="left")
        ttk.Label(t, text=tr("anticheat_warn"),
                  style="Muted.TLabel", foreground=AMBER,
                  wraplength=860).pack(anchor="w", pady=(6, 0))

    def _wheel(self, e):
        try:
            if self.canvas.winfo_viewable():
                self.canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")
        except tk.TclError:
            pass

    # ---------------------------------------------------------- selection --
    def selected_exe(self):
        return self._selected

    def select(self, exe):
        self._selected = exe
        for path, card in self.cards.items():
            card.set_selected(path == exe)
        meta = (self.ctl.cfg.get("games") or {}).get(exe or "")
        if meta:
            self.mode_var.set(MODE_NAMES.get(meta["mode"], tr("mode_auto")))

    def _chosen_mode(self):
        v = self.mode_var.get().lower()
        return None if v in ("авто", "auto") else v

    # ------------------------------------------------------------ actions --
    def _add_manual(self):
        p = filedialog.askopenfilename(title=tr("pick_exe"),
                                       filetypes=[("exe", "*.exe")])
        if p:
            self.ctl.submit(self.ctl.add_game, p, None, None, None, None)

    def _enable(self):
        exe = self.selected_exe()
        if not exe:
            self.ui.log("mgr", tr("pick_game"))
            return
        launch = None
        for g in self.scanned:
            if g.exe == exe:
                launch = g.launch_exe if g.via_wrapper else None
        self.ctl.submit(self._enable_worker, exe, self._chosen_mode(),
                        launch)

    def _enable_worker(self, exe, mode, launch_exe=None):
        ok, msg = self.ctl.add_game(exe, mode, launch_exe=launch_exe)
        if not ok:
            return False, msg
        return self.ctl.game_deploy(exe)

    def _launch(self):
        exe = self.selected_exe()
        if not exe:
            self.ui.log("mgr", tr("pick_game"))
            return
        if exe not in (self.ctl.cfg.get("games") or {}):
            launch = None
            for g in self.scanned:
                if g.exe == exe:
                    launch = g.launch_exe if g.via_wrapper else None
            self.ctl.submit(self._enable_worker, exe, self._chosen_mode(),
                            launch)
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
        self.scan_info.config(text=tr("scanning"))
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
            self.scan_info.config(text=tr("scan_failed", payload))
            return
        self.scanned = payload
        self.scan_info.config(text=tr("games_found", len(self.scanned)))
        self.ui.log("mgr", "game scan: %d games (grouped, launchers "
                    "resolved)" % len(self.scanned))
        self.cards_fill()

    def cards_fill(self):
        for w in self.grid_frame.winfo_children():
            w.destroy()
        self.cards = {}
        known = self.ctl.cfg.get("games") or {}
        entries = []
        for g in self.scanned:
            entries.append(dict(exe=g.exe, name=g.name, mode=g.mode,
                                arch=g.arch, source=g.source,
                                launch_exe=(g.launch_exe
                                            if g.via_wrapper else None),
                                anticheat=g.anticheat, saved=False))
        for exe, meta in known.items():
            if all(e["exe"] != exe for e in entries):
                entries.append(dict(
                    exe=exe, name=meta["name"] + " (%s)" % tr("saved_mark"),
                    mode=meta["mode"], arch=meta.get("arch"), source="saved",
                    launch_exe=meta.get("launch_exe"), anticheat=False,
                    saved=True))
        for i, e in enumerate(entries):
            card = GameCard(self, self.grid_frame, **e)
            card.frame.grid(row=i // CARD_COLS, column=i % CARD_COLS,
                            padx=6, pady=6, sticky="n")
            self.cards[e["exe"]] = card
        if self._selected in self.cards:
            self.cards[self._selected].set_selected(True)

    # ------------------------------------------------------------- render --
    def render(self, snap):
        games = snap.get("games") or {}
        for exe, card in self.cards.items():
            meta = games.get(exe)
            if meta:
                if meta["running"]:
                    state, color = tr("state_running"), GREEN
                    if meta["paused"]:
                        state += tr("state_paused")
                elif meta["dll"] == "deployed":
                    state, color = tr("state_enabled"), ACCENT
                elif meta["dll"] in ("foreign", "partial"):
                    state, color = tr("state_foreign"), RED
                else:
                    state, color = tr("state_added"), MUTED
            else:
                state, color = tr("state_not_added"), MUTED
            if card.anticheat and not (meta and meta["running"]):
                state, color = tr("state_anticheat"), AMBER
            card.set_state(state, color)
