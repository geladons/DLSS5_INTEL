# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.ui_games - the Games tab: found games as ICON CARDS (owner feedback:
# "a list of 180 exe files is unreadable - show the actual games as icons").
# One card per real game (gamescan groups candidates; only API-positive
# install roots become cards). A card shows the exe icon, the game name,
# the detected graphics API, the launch wrapper (pirate/GOG stubs) and the
# live status (enabled / running / anti-cheat risk).
#
# Layout: scan row | centered expanding card grid | action row | bordered
# per-game options strip (mode/window/args/fg) | anti-cheat warning.
# The card grid scrolls via the app-level wheel router (ManagerUI.
# register_wheel_region) and reflows its column count to the canvas width.
#
# All heavy work (drive scan, deploy, launch) runs off the Tk thread; this
# class only renders and queues.
# ============================================================================
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, ttk

from .gamescache import load_cache, save_cache
from .i18n import tr
from .icons import IconCache

MODE_NAMES = {"dx9": "DX9", "dx11": "DX11", "dx12": "DX12",
              "vulkan": "Vulkan"}
MODE_COLORS = {"dx9": "#d29922", "dx11": "#58a6ff", "dx12": "#9a6bff",
               "vulkan": "#3fb950", None: "#8a91a5"}

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
BORDER = "#2a3040"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

CARD_W = 150
CARD_H = 172
CARD_PAD = 6
CARD_COLS = 4          # initial column count; reflows to the canvas width


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
        self._cols = CARD_COLS
        self.icons = IconCache(size=48)
        self._build(nb)
        cached = load_cache()
        if cached:
            self.scanned = cached
            self.scan_info.config(text=tr("games_found", len(self.scanned)))
            self.cards_fill()
        else:
            self.scan_info.config(text="")

    def _build(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text=tr("tab_games"))
        self.tab_frame = t
        top = ttk.Frame(t)
        top.pack(fill="x", pady=(0, 4))
        ttk.Button(top, text=tr("rescan"), style="Accent.TButton",
                   command=self.scan_start).pack(side="left")
        ttk.Button(top, text=tr("add_manual"),
                   command=self._add_manual).pack(side="left", padx=6)

        self.scan_info = ttk.Label(top, text="",
                                   style="Muted.TLabel")
        self.scan_info.pack(side="left", padx=10)

        # Scrollable card grid: expands with the window (no fixed height),
        # the card block stays horizontally centered and the column count
        # reflows to the canvas width (_canvas_resized).
        wrap = tk.Frame(t, bg=BG)
        wrap.pack(fill="both", expand=True, pady=(0, 6))
        self.canvas = tk.Canvas(wrap, bg=BG, highlightthickness=0)
        vsb = ttk.Scrollbar(wrap, orient="vertical",
                            command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.grid_frame = tk.Frame(self.canvas, bg=BG)
        self._grid_win = self.canvas.create_window(
            (0, 0), window=self.grid_frame, anchor="n")
        self.grid_frame.bind("<Configure>", lambda _e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", self._canvas_resized)
        # Mouse wheel: routed app-wide by ManagerUI.register_wheel_region
        # (a second bind_all("<MouseWheel>") used to replace ours - the
        # settings tab was built later and silently killed grid scroll).
        # The whole tab is the region, so the wheel works over cards, the
        # scrollbar and the option strip alike. Unit-test stub UIs have
        # no router; the Enter/Leave fallback keeps this tab contained.
        register = getattr(self.ui, "register_wheel_region", None)
        if callable(register):
            register(t, self.canvas)
        else:
            self.canvas.bind("<Enter>", self._wheel_bind)
            self.canvas.bind("<Leave>", self._wheel_unbind)

        # Action row: the things you DO to the selected card. Pause/Resume
        # acts on the running game (global processing toggle) - parked at
        # the right end, away from the per-game options below.
        act = ttk.Frame(t)
        act.pack(fill="x", pady=(0, 4))
        ttk.Button(act, text=tr("enable"), style="Accent.TButton",
                   command=self._enable).pack(side="left")
        ttk.Button(act, text=tr("play"),
                   command=self._launch).pack(side="left", padx=6)
        ttk.Button(act, text=tr("disable"),
                   command=self._disable).pack(side="left")
        ttk.Button(act, text=tr("remove"), style="Danger.TButton",
                   command=self._remove).pack(side="left", padx=6)
        ttk.Button(act, text=tr("pause_resume"),
                   command=lambda: self.ctl.submit(
                       self.ctl.processing_toggle)).pack(side="right")

        # Per-game options strip (bordered, clearly "for <game>"): API mode
        # override, windowed flag, explicit launch args (win over the
        # windowed checkbox) and a framegen override (auto = the global
        # switch). Args/fg save on focus-out; mode/window are carried by
        # the Enable/Play paths - a re-register (add_game writes a fresh
        # meta) must not silently drop them, so those paths carry the
        # current field values into the new meta too.
        ext = tk.Frame(t, bg=PANEL, highlightthickness=1,
                       highlightbackground=BORDER)
        ext.pack(fill="x")
        inner = tk.Frame(ext, bg=PANEL)
        inner.pack(fill="x", padx=10, pady=7)
        self.ext_who = tk.Label(inner, text=tr("extras_none"), bg=PANEL,
                                fg=MUTED, font=("Segoe UI", 8, "bold"))
        self.ext_who.pack(side="left", padx=(0, 10))
        tk.Label(inner, text=tr("mode_label"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        self.mode_var = tk.StringVar(value=tr("mode_auto"))
        self.mode_box = ttk.Combobox(
            inner, textvariable=self.mode_var, width=7, state="readonly",
            values=(tr("mode_auto"), "DX9", "DX11", "DX12", "Vulkan"))
        self.mode_box.pack(side="left", padx=(4, 12))
        # Windowed checkbox: best effort - the launcher appends
        # gamelaunch.WINDOW_ARGS ("-windowed -window") when set and no
        # custom launch_args exist. Remembered per game.
        self.window_var = tk.BooleanVar(value=False)
        self.window_chk = ttk.Checkbutton(
            inner, text=tr("window_cb"), variable=self.window_var,
            style="Panel.TCheckbutton")
        self.window_chk.pack(side="left", padx=(0, 12))
        tk.Label(inner, text=tr("game_args"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        self.args_var = tk.StringVar()
        self.args_entry = ttk.Entry(inner, textvariable=self.args_var,
                                    width=30)
        self.args_entry.pack(side="left", padx=(4, 12))
        self.args_entry.bind("<FocusOut>", lambda e: self._save_extras())
        tk.Label(inner, text=tr("game_fg"), bg=PANEL, fg=FG,
                 font=("Segoe UI", 8)).pack(side="left")
        self.fg_var = tk.StringVar(value=tr("fg_auto"))
        self.fg_box = ttk.Combobox(
            inner, textvariable=self.fg_var, width=6, state="readonly",
            values=(tr("fg_auto"), tr("fg_on"), tr("fg_off")))
        self.fg_box.pack(side="left", padx=(4, 0))
        self.fg_box.bind("<<ComboboxSelected>>", lambda e: self._save_extras())
        self._extras_state(tk.DISABLED)

        ttk.Label(t, text=tr("anticheat_warn"),
                  style="Muted.TLabel", foreground=AMBER,
                  wraplength=800).pack(anchor="w", pady=(6, 0))

    def _wheel(self, e):
        """Standalone fallback handler (test stubs only - the real UI uses
        ManagerUI's wheel router)."""
        try:
            if self.canvas.winfo_viewable():
                self.canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")
        except tk.TclError:
            pass

    def _wheel_bind(self, _e):
        self.canvas.bind_all("<MouseWheel>", self._wheel)

    def _wheel_unbind(self, e):
        """Fallback only: drop the global wheel binding when the pointer
        leaves the canvas rect for real - stepping onto a card (an
        embedded child) fires a spurious <Leave> whose root coords are
        still inside the canvas."""
        try:
            x0, y0 = self.canvas.winfo_rootx(), self.canvas.winfo_rooty()
            if (x0 <= e.x_root < x0 + self.canvas.winfo_width() and
                    y0 <= e.y_root < y0 + self.canvas.winfo_height()):
                return
        except tk.TclError:
            pass
        self.canvas.unbind_all("<MouseWheel>")

    def _canvas_resized(self, e):
        """Re-center the card block and reflow the grid: as many CARD_W
        columns as fit the new canvas width."""
        self.canvas.coords(self._grid_win, e.width // 2, 0)
        self.canvas.itemconfigure(self._grid_win, anchor="n")
        cols = max(1, (e.width - 8) // (CARD_W + CARD_PAD * 2))
        if cols != self._cols:
            self._cols = cols
            self._regrid()

    def _regrid(self):
        """Re-place the existing cards at the current column count."""
        for i, card in enumerate(self.cards.values()):
            card.frame.grid_configure(row=i // self._cols,
                                      column=i % self._cols,
                                      padx=CARD_PAD, pady=CARD_PAD,
                                      sticky="n")

    # ---------------------------------------------------------- selection --
    def selected_exe(self):
        return self._selected

    def _extras_state(self, state):
        """Enable/grey the per-game options strip; state is tk.NORMAL or
        tk.DISABLED. The comboboxes are readonly so 'normal' keeps them
        pickable-but-not-typable."""
        self.args_entry.config(state=state)
        combo = "readonly" if state == tk.NORMAL else "disabled"
        self.fg_box.config(state=combo)
        self.mode_box.config(state=combo)
        self.window_chk.state(["!disabled"] if state == tk.NORMAL
                              else ["disabled"])

    def select(self, exe):
        self._selected = exe
        for path, card in self.cards.items():
            card.set_selected(path == exe)
        meta = (self.ctl.cfg.get("games") or {}).get(exe or "") or {}
        if exe:
            name = meta.get("name") or \
                os.path.splitext(os.path.basename(exe))[0]
            if len(name) > 38:          # long titles stay inside the strip
                name = name[:35] + "..."
            self.ext_who.config(text=tr("extras_for", name), fg=FG)
        else:
            self.ext_who.config(text=tr("extras_none"), fg=MUTED)
        self._extras_state(tk.NORMAL if exe else tk.DISABLED)
        if meta:
            self.mode_var.set(MODE_NAMES.get(meta["mode"], tr("mode_auto")))
            self.window_var.set(bool(meta.get("window")))
        else:
            self.mode_var.set(tr("mode_auto"))
            self.window_var.set(False)
        self.args_var.set(meta.get("launch_args") or "")
        fgm = meta.get("framegen")
        self.fg_var.set(tr("fg_on") if fgm is True else
                        tr("fg_off") if fgm is False else tr("fg_auto"))

    def _chosen_mode(self):
        v = self.mode_var.get().lower()
        return None if v in ("авто", "auto") else v

    def _chosen_fg(self):
        v = self.fg_var.get().strip().lower()
        if v in (tr("fg_on").lower(), "on"):
            return True
        if v in (tr("fg_off").lower(), "off"):
            return False
        return None

    def _save_extras(self):
        """Persist the extras row into the selected game's meta. Called on
        focus-out/combobox pick - re-registering the exe later re-reads
        these keys, unlike a hand-edited config.json."""
        exe = self._selected
        if not exe:
            return
        games = self.ctl.cfg.get("games") or {}
        meta = games.get(exe)
        if not isinstance(meta, dict):
            return
        args = self.args_var.get().strip()
        fg = self._chosen_fg()
        changed = False
        if (meta.get("launch_args") or "") != args:
            if args:
                meta["launch_args"] = args
            else:
                meta.pop("launch_args", None)
            changed = True
        if meta.get("framegen") != fg:
            if fg is None:
                meta.pop("framegen", None)
            else:
                meta["framegen"] = fg
            changed = True
        if changed:
            self.ctl.cfg.save()

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
                        launch, self.args_var.get().strip(),
                        self._chosen_fg(),
                        bool(self.window_var.get()))

    def _enable_worker(self, exe, mode, launch_exe=None, args="",
                       framegen=None, window=False):
        ok, msg = self.ctl.add_game(exe, mode, launch_exe=launch_exe)
        if not ok:
            return False, msg
        # add_game wrote a fresh {mode, arch, name} meta - carry the extras
        # row the user typed before hitting Enable (window, launch args,
        # framegen override) into it or they are silently lost.
        games = self.ctl.cfg.get("games") or {}
        meta = games.get(exe)
        if isinstance(meta, dict):
            if window:
                meta["window"] = True
            if args:
                meta["launch_args"] = args
            if framegen is not None:
                meta["framegen"] = framegen
            self.ctl.cfg.save()
        return self.ctl.game_deploy(exe)

    def _launch(self):
        exe = self.selected_exe()
        if not exe:
            self.ui.log("mgr", tr("pick_game"))
            return
        reg = None
        window = bool(self.window_var.get())
        games = self.ctl.cfg.get("games") or {}
        if exe in games:
            # Only the window bit is owned by the launch click. The extras
            # row persists itself on focus-out/combobox-pick; writing the
            # field values again here could strip an override the moment a
            # stale "auto" is showing (that exact wipe killed the GTA IV
            # framegen=false during testing).
            if games[exe].get("window") != window:
                games[exe]["window"] = window
                self.ctl.cfg.save()
        else:
            launch = None
            for g in self.scanned:
                if g.exe == exe:
                    launch = g.launch_exe if g.via_wrapper else None
            reg = {"mode": self._chosen_mode(), "launch_exe": launch,
                   "window": window}
            args = self.args_var.get().strip()
            if args:
                reg["launch_args"] = args
            fg = self._chosen_fg()
            if fg is not None:
                reg["framegen"] = fg
        self.ctl.launch_game_async(exe, register=reg)

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
        save_cache(self.scanned)
        self.scan_info.config(text=tr("games_found", len(self.scanned)))
        self.ui.log("mgr", "game scan: %d games (cached, launchers "
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
            card.frame.grid(row=i // self._cols, column=i % self._cols,
                            padx=CARD_PAD, pady=CARD_PAD, sticky="n")
            self.cards[e["exe"]] = card
        if self._selected in self.cards:
            self.cards[self._selected].set_selected(True)
        self.canvas.yview_moveto(0)      # a refilled grid starts at the top

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
                    # Cosmetic warning only, never a blocker: deploy()
                    # parks foreign dlls as .m13bak before writing ours.
                    state, color = tr("state_foreign"), RED
                else:
                    state, color = tr("state_added"), MUTED
            else:
                state, color = tr("state_not_added"), MUTED
            if card.anticheat and not (meta and meta["running"]):
                state, color = tr("state_anticheat"), AMBER
            card.set_state(state, color)
