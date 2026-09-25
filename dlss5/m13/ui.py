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
import time
import tkinter as tk
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

from .controller import M13Controller
from .logtail import LogHub
from .overlay import ControlOverlay, hotkey_label
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
        self.root = tk.Tk()
        self.root.title("DLSS 5 Менеджер")
        self.root.geometry("1024x760")
        self.root.minsize(900, 640)
        self.root.configure(bg=BG)
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
        # hands-free bring-up + first game scan (worker thread)
        self.ctl.submit(self.ctl.autosetup)
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
        ttl = tk.Frame(head, bg=BG)
        ttl.pack(side="left")
        self.title_lbl = tk.Label(ttl, text="DLSS 5 Менеджер", bg=BG, fg=FG,
                                  font=("Segoe UI", 14, "bold"))
        self.title_lbl.pack(anchor="w")
        tk.Label(ttl, text="реальная 71-блочная нейросеть, живая, на твоей "
                           "Arc Pro B50", bg=BG, fg=MUTED,
                 font=("Segoe UI", 8)).pack(anchor="w")
        ttk.Button(head, text="Игровой оверлей (%s)" % hotkey_label(
            self.ctl.cfg.get("overlay_hotkey")),
            command=self.overlay.toggle).pack(side="right")

        bar = tk.Frame(self.root, bg=PANEL,
                       highlightthickness=1, highlightbackground=BORDER)
        bar.pack(fill="x", padx=12, pady=4)
        inner = tk.Frame(bar, bg=PANEL)
        inner.pack(fill="x", padx=10, pady=8)
        self.dot = tk.Label(inner, text="●", font=("Segoe UI", 10), bg=PANEL)
        self.dot.pack(side="left")
        self.st_daemon = tk.Label(inner, text="демон: ...", bg=PANEL,
                                  fg=MUTED, font=("Segoe UI", 9, "bold"))
        self.st_daemon.pack(side="left", padx=(6, 14))
        self.btn_start = ttk.Button(inner, text="Запустить демон",
                                    style="Accent.TButton",
                                    command=self._daemon_start)
        self.btn_start.pack(side="left")
        ttk.Button(inner, text="Стоп", style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.daemon_stop)).pack(side="left", padx=6)
        tk.Label(inner, text="сила", bg=PANEL, fg=MUTED).pack(side="left",
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

    def _draw_logo(self, color):
        self.logo.delete("all")
        self.logo.create_polygon(15, 2, 28, 15, 15, 28, 2, 15, fill=color,
                                 outline="")
        self.logo.create_polygon(15, 8, 22, 15, 15, 22, 8, 15, fill=BG,
                                 outline="")

    def _logo_click(self, _e):
        """Hidden easter egg: five clicks on the diamond."""
        self._egg_clicks += 1
        if self._egg_clicks < EASTER_EGG_CLICKS:
            return
        self._egg_clicks = 0
        self.title_lbl.config(text="DLSS 5 «Пятёрочка»")
        self.log("mgr", "ПАСХАЛКА: нейросеть обучена на 10 000 часах GTA IV "
                        "и одном очень терпеливом владельце.")
        colors = (ACCENT, ACCENT2, GREEN, AMBER, RED, "#ff6ec7")

        def flash(i=0):
            if i >= 12:
                self._draw_logo(ACCENT)
                self.title_lbl.config(text="DLSS 5 Менеджер")
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
        tk.Label(row, text="Автонастройка:", bg=PANEL, fg=MUTED,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.steps = {}
        for key, label in (("weights", "Веса"), ("layers", "Слои Vulkan"),
                           ("daemon", "Демон"), ("game", "Игра готова")):
            lbl = tk.Label(row, text=label, bg=PANEL, fg=MUTED,
                           font=("Segoe UI", 9), padx=10)
            lbl.pack(side="left")
            self.steps[key] = lbl
        self.hint = tk.Label(box, text="", bg=PANEL, fg=AMBER,
                             font=("Segoe UI", 9), anchor="w")
        self.hint.pack(fill="x", padx=10, pady=(0, 8))

    # --------------------------------------------------------- Screen tab --
    def _tab_screen(self, nb):
        t = ttk.Frame(nb, padding=10)
        nb.add(t, text="  Экран  ")
        ttk.Label(t, text="Полноэкранный оверлей рабочего стола (смотри "
                          "через трансляцию Moonlight). ВАЖНО: демон и "
                          "оверлей экрана не живут вместе - каждому нужно "
                          "~12 ГБ видеопамяти из 16. При запуске одного "
                          "второй останавливается автоматически.",
                  style="Muted.TLabel", wraplength=900,
                  justify="left").pack(anchor="w")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=8)
        ttk.Button(row, text="Запустить оверлей экрана",
                   style="Accent.TButton",
                   command=self._screen_start).pack(side="left")
        ttk.Button(row, text="Стоп", style="Danger.TButton",
                   command=lambda: self.ctl.submit(
                       self.ctl.screen_stop)).pack(side="left", padx=6)
        ttk.Label(row, text="горячие клавиши: CTRL+ALT+X скрыть/показать, "
                            "CTRL+ALT+Q выход",
                  style="Muted.TLabel").pack(side="left", padx=14)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=(4, 2))
        ttk.Label(row2, text="Режим окна - заголовок содержит:").pack(
            side="left")
        self.win_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.win_var, width=30).pack(
            side="left", padx=6)
        ttk.Button(row2, text="Запустить на окне",
                   command=self._screen_window_start).pack(side="left")
        self.screen_info = ttk.Label(t, text="оверлей: выкл",
                                     style="Muted.TLabel")
        self.screen_info.pack(anchor="w", pady=6)
        self.screen_tail = ttk.Label(
            t, text="Прогрев 1440p: загрузка весов ~30 с, первый кадр до "
                    "минуты - это загрузка, а не зависание. Для быстрого "
                    "демо используй режим окна выше.",
            style="Muted.TLabel", wraplength=900, justify="left")
        self.screen_tail.pack(anchor="w")

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
        nb.add(t, text="  Настройки  ")
        row = ttk.Frame(t)
        row.pack(anchor="w", pady=4, fill="x")
        ttk.Label(row, text="веса (.safetensors):").pack(side="left")
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
        ttk.Button(row, text="Обзор...", command=browse_weights).pack(
            side="left")
        ttk.Button(row, text="Сохранить", command=lambda: (
            self.ctl.cfg.set("weights_path", self.weights_var.get()),
            self.log("mgr", "weights path saved"))).pack(side="left", padx=6)

        row2 = ttk.Frame(t)
        row2.pack(anchor="w", pady=6)
        ttk.Button(row2, text="Зарегистрировать слои Vulkan (HKCU)",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_register)).pack(side="left")
        ttk.Button(row2, text="Снять регистрацию",
                   command=lambda: self.ctl.submit(
                       self.ctl.layers_unregister)).pack(side="left", padx=6)
        self.layers_info = ttk.Label(row2, text="слои: ?",
                                     style="Muted.TLabel")
        self.layers_info.pack(side="left", padx=10)

        opts = ttk.Frame(t)
        opts.pack(anchor="w", pady=6)
        self.freeze_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        ttk.Checkbutton(opts, text="Замораживать кадр, пока оверлей открыт "
                                   "(фото-режим с живым превью)",
                        variable=self.freeze_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_freeze", bool(self.freeze_var.get()))
                        ).pack(anchor="w")
        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        ttk.Checkbutton(opts, text="Ставить обработку на паузу при открытии "
                                   "оверлея",
                        variable=self.ap_var,
                        command=lambda: self.ctl.cfg.set(
                            "overlay_autopause", bool(self.ap_var.get()))
                        ).pack(anchor="w")

        ttk.Label(t, text="Горячая клавиша оверлея: %s (меняется в "
                          "config.json)" % self.ctl.cfg.get("overlay_hotkey"),
                  style="Muted.TLabel").pack(anchor="w", pady=(10, 2))
        ttk.Label(t, text="Конфиг: %s" % self.ctl.cfg.path,
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
            txt = "работает (pid %s)" % snap["daemon_pid"]
            g, f = snap.get("daemon_gain"), snap.get("daemon_frames")
            if g is not None:
                txt += " - сила %.2f, %d кадров" % (g, f or 0)
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
                txt += " [занят]"
            self.dot.config(text="●", fg=GREEN)
            self.st_daemon.config(text="демон: " + txt, fg=GREEN)
            self.daemon_info.config(text="NRCT live control OK"
                                    if not snap.get("daemon_err") else
                                    snap["daemon_err"])
        else:
            self.dot.config(text="●", fg=RED)
            self.st_daemon.config(text="демон: остановлен", fg=RED)
            self.daemon_info.config(text="")

        lx = snap.get("layers_x64"), snap.get("layers_x86")
        self.layers_info.config(text="слои: x64 %s / x86 %s" % (
            "OK" if lx[0] else "НЕТ", "OK" if lx[1] else "НЕТ"))
        screen_up = snap.get("screen_pid") is not None
        self.screen_info.config(text="оверлей: %s" % (
            "РАБОТАЕТ (pid %s, греется - первые кадры долгие)"
            % snap["screen_pid"] if screen_up else "выкл"))
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
            base = {"weights": "Веса", "layers": "Слои Vulkan",
                    "daemon": "Демон", "game": "Игра готова"}[key]
            self.steps[key].config(text=("✔ " if ok else "· ") + base,
                                   fg=GREEN if ok else MUTED)
        if not weights:
            hint, color = ("Не найдены веса - вкладка «Настройки», укажи "
                           "dlssnr-logical.safetensors", AMBER)
        elif not layers or not daemon:
            hint, color = "Настраиваю сам, несколько секунд...", AMBER
        elif not game:
            hint, color = ("Выбери игру карточкой на вкладке «Игры» и нажми "
                           "«Включить DLSS 5»", AMBER)
        else:
            hint, color = ("Всё готово! Жми «Играть»; в игре %s открывает "
                           "панель управления." % hotkey_label(
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
