# ============================================================================
# m13.overlay - the in-game control overlay (hotkey-invoked, default
# CTRL+ALT+G). NOT a normal window anymore: a frameless, always-on-top,
# semi-transparent panel centered over the screen so it floats above the
# game instead of opening as a separate app window. Drag by the title strip.
#
# Controls, all LIVE without leaving the game:
#   [PAUSE]/[RESUME]  processing toggle on the active path (file channels;
#                     pause = passthrough at full fps, the frame you saw is
#                     the last processed one)
#   gain slider       live NRCT push to m11d (next processed frame); in
#                     screen mode it restarts the overlay with the new gain
#   auto-pause        optional: pause processing the moment the overlay opens
#
# Threading: the overlay never does I/O itself. It renders ctl.snapshot()
# (monitor thread) and routes every action through ctl.submit() (worker).
#
# The show/hide hotkey is POLLED via GetAsyncKeyState (edge-triggered): no
# message-only window needed, and physical presses are what the owner uses.
# Injected modifiers are unreliable on this host, so we never synthesize
# keystrokes - the owner presses the combo.
# ============================================================================
import ctypes
import time
import tkinter as tk

from .processes import suspend_pid, resume_pid
from .gamelaunch import mode_paused as gamelaunch_mode_paused

VK = {"control": 0x11, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
POLL_MS = 120
DEBOUNCE_MS = 250
STATUS_MS = 500

# Extended window styles applied after mapping: NOACTIVATE is THE fix for
# "clicking the overlay minimizes the game" - the panel never steals focus
# from the game window; TOOLWINDOW keeps it out of Alt-Tab.
GWL_EXSTYLE = -20
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080

# dark theme (shared palette with ui.py)
BG = "#16181d"
PANEL = "#22252d"
FG = "#d7dae0"
MUTED = "#8b919e"
ACCENT = "#4f8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"


def parse_hotkey(spec):
    """'control+alt+g' -> (mods_mask, vk). Defaults to ctrl+alt+g."""
    mods = 0
    key = "g"
    parts = [p.strip().lower() for p in (spec or "").split("+") if p.strip()]
    if parts:
        key = parts[-1]
        for p in parts[:-1]:
            if p in VK:
                mods |= {0x11: 0x0002, 0x12: 0x0001, 0x10: 0x0004}[VK[p]]
    vk = ord(key[0].upper()) if key else ord("G")
    return mods, vk


def hotkey_label(spec):
    return (spec or "control+alt+g").upper().replace("CONTROL", "CTRL")


class HotkeyPoller:
    """Edge-triggered global combo polling."""

    def __init__(self, spec, on_fire, poll_ms=POLL_MS):
        self.mods, self.vk = parse_hotkey(spec)
        self.on_fire = on_fire
        self.poll_ms = poll_ms
        self._armed = True
        self._user32 = ctypes.windll.user32
        self._tk = None

    def _down(self, vk):
        return bool(self._user32.GetAsyncKeyState(vk) & 0x8000)

    def poll(self):
        combo = self._down(self.vk)
        if combo and self.mods:
            combo = all(self._down(vk) for bit, vk in
                        ((0x0002, VK["control"]), (0x0001, VK["alt"]),
                         (0x0004, VK["shift"])) if self.mods & bit)
        if combo and self._armed:
            self._armed = False
            self.on_fire()
        elif not combo:
            self._armed = True
        if self._tk is not None:
            self._tk.after(self.poll_ms, self.poll)

    def attach(self, tk_widget):
        self._tk = tk_widget
        tk_widget.after(self.poll_ms, self.poll)


class ControlOverlay:
    """Frameless always-on-top control panel floating over the game."""

    WIDTH, HEIGHT = 430, 320

    def __init__(self, master, ctl, on_log):
        self.ctl = ctl
        self.on_log = on_log
        self.win = tk.Toplevel(master)
        self.win.overrideredirect(True)          # frameless: no OS window look
        self.win.attributes("-topmost", True)
        self.win.attributes("-alpha", 0.94)
        self.win.configure(bg=BG, highlightthickness=1,
                           highlightbackground=ACCENT)
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.hide)
        self.win.withdraw()
        self._after_id = None
        self._drag = None
        self._frozen_pid = None      # game pid suspended while we are open
        self._pending_push = None    # (value, time) of an in-flight push
        self._build()
        self.poller = HotkeyPoller(ctl.cfg.get("overlay_hotkey"), self.toggle)
        self.poller.attach(master)

    def _noactivate(self):
        """WS_EX_NOACTIVATE|TOOLWINDOW so clicking us never minimizes the
        game (focus stays on the game window)."""
        try:
            hwnd = int(self.win.wm_frame(), 16)
            u32 = ctypes.windll.user32
            style = u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
            u32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                                  style | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
        except (ValueError, OSError):
            pass

    # ------------------------------------------------------------- build ---
    def _build(self):
        w = self.win
        # title strip (drag handle)
        strip = tk.Frame(w, bg=PANEL, height=30)
        strip.pack(fill="x")
        strip.pack_propagate(False)
        self.title_lbl = tk.Label(strip, text="DLSS 5", bg=PANEL, fg=FG,
                                  font=("Segoe UI", 10, "bold"))
        self.title_lbl.pack(side="left", padx=10)
        close = tk.Label(strip, text="X", bg=PANEL, fg=MUTED,
                         font=("Segoe UI", 10, "bold"), padx=10, cursor="hand2")
        close.pack(side="right")
        close.bind("<Button-1>", lambda _e: self.hide())
        close.bind("<Enter>", lambda _e: close.config(fg=RED))
        close.bind("<Leave>", lambda _e: close.config(fg=MUTED))
        for widget in (strip, self.title_lbl):
            widget.bind("<ButtonPress-1>", self._drag_start)
            widget.bind("<B1-Motion>", self._drag_move)

        body = tk.Frame(w, bg=BG, padx=14, pady=10)
        body.pack(fill="both", expand=True)

        # processing toggle (the big one)
        self.toggle_btn = tk.Button(
            body, text="PAUSE PROCESSING", font=("Segoe UI", 10, "bold"),
            bg=ACCENT, fg="#ffffff", activebackground="#3a70d6",
            activeforeground="#ffffff", relief="flat", bd=0, pady=7,
            cursor="hand2", command=self._toggle_processing)
        self.toggle_btn.pack(fill="x")

        self.mode_lbl = tk.Label(body, text="", bg=BG, fg=MUTED,
                                 font=("Segoe UI", 8))
        self.mode_lbl.pack(anchor="w", pady=(3, 8))

        # gain
        grow = tk.Frame(body, bg=BG)
        grow.pack(fill="x")
        tk.Label(grow, text="Gain", bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        self.gain_val = tk.Label(grow, text="1.00", bg=BG, fg=ACCENT,
                                 font=("Consolas", 10, "bold"), width=5)
        self.gain_val.pack(side="right")
        self.var = tk.DoubleVar(value=self.ctl.cfg.get("gain"))
        self._suppress_scale = None   # programmatic set echo suppression:
        self._scale_dragging = False  # Tk fires the command DEFERRED
        self.scale = tk.Scale(
            body, from_=0.0, to=2.0, resolution=0.05, orient="horizontal",
            variable=self.var, command=self._debounced_push,
            bg=BG, fg=FG, troughcolor=PANEL, highlightthickness=0, bd=0,
            activebackground=ACCENT, showvalue=False, sliderrelief="flat")
        self.scale.pack(fill="x", pady=(0, 4))
        self.scale.bind("<ButtonPress-1>",
                        lambda _e: setattr(self, "_scale_dragging", True))
        self.scale.bind("<ButtonRelease-1>",
                        lambda _e: setattr(self, "_scale_dragging", False))

        prow = tk.Frame(body, bg=BG)
        prow.pack(fill="x", pady=(0, 4))
        tk.Button(prow, text="Reset 1.0", font=("Segoe UI", 8), bg=PANEL,
                  fg=FG, activebackground="#2e323c", activeforeground=FG,
                  relief="flat", bd=0, padx=8, cursor="hand2",
                  command=self._reset_gain).pack(side="left")
        self.ap_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_autopause")))
        tk.Checkbutton(prow, text="auto-pause on open", variable=self.ap_var,
                       command=self._save_autopause, bg=BG, fg=MUTED,
                       selectcolor=PANEL, activebackground=BG,
                       activeforeground=FG, font=("Segoe UI", 8),
                       cursor="hand2").pack(side="right")

        frow = tk.Frame(body, bg=BG)
        frow.pack(fill="x", pady=(0, 6))
        self.fr_var = tk.BooleanVar(
            value=bool(self.ctl.cfg.get("overlay_freeze")))
        tk.Checkbutton(frow, text="freeze game while open (one frame stays)",
                       variable=self.fr_var, command=self._save_freeze,
                       bg=BG, fg=MUTED, selectcolor=PANEL,
                       activebackground=BG, activeforeground=FG,
                       font=("Segoe UI", 8), cursor="hand2").pack(side="left")

        self.status = tk.Label(body, text="daemon: ?", bg=BG, fg=MUTED,
                               font=("Consolas", 8), justify="left")
        self.status.pack(anchor="w")

        tk.Label(body, text="%s - hide" % hotkey_label(
            self.ctl.cfg.get("overlay_hotkey")), bg=BG, fg="#565b66",
            font=("Segoe UI", 8)).pack(side="bottom", pady=(8, 0))

    # -------------------------------------------------------------- drag ---
    def _drag_start(self, e):
        self._drag = (e.x_root - self.win.winfo_x(),
                      e.y_root - self.win.winfo_y())

    def _drag_move(self, e):
        if self._drag:
            self.win.geometry("+%d+%d" % (e.x_root - self._drag[0],
                                          e.y_root - self._drag[1]))

    # ----------------------------------------------------------- actions ---
    def _toggle_processing(self):
        self.ctl.submit(self.ctl.processing_toggle)

    def _debounced_push(self, _val):
        g = round(float(self.var.get()), 3)
        if self._suppress_scale is not None and \
                abs(g - self._suppress_scale) < 1e-9:
            self._suppress_scale = None      # echo of a status-driven sync
            return
        self._suppress_scale = None
        self.gain_val.config(text="%.2f" % g)
        self._pending_push = (g, time.time())
        if self._after_id:
            self.win.after_cancel(self._after_id)
        self._after_id = self.win.after(DEBOUNCE_MS, self._push)

    def _push(self):
        self._after_id = None
        g = round(float(self.var.get()), 3)
        self.ctl.submit(self.ctl.set_gain, g)

    def _reset_gain(self):
        self.var.set(1.0)
        self._debounced_push(1.0)

    def _save_autopause(self):
        self.ctl.cfg.set("overlay_autopause", bool(self.ap_var.get()))

    def _save_freeze(self):
        self.ctl.cfg.set("overlay_freeze", bool(self.fr_var.get()))

    # ------------------------------------------------------------ status ---
    def set_status(self, text):
        try:
            self.status.config(text=text)
        except tk.TclError:
            pass

    def _refresh(self):
        """Render the monitor snapshot; runs only while visible."""
        if not self.win.winfo_viewable():
            return
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        names = {"dx9": "DX9 game", "dx11": "DX11 game", "dx12": "DX12 game",
                 "vulkan": "Vulkan game", "screen": "Screen mode"}
        paused = self.ctl.processing_paused()
        if paused is True:
            self.toggle_btn.config(text="RESUME PROCESSING", bg=GREEN,
                                   activebackground="#2ea043")
        elif paused is False:
            self.toggle_btn.config(text="PAUSE PROCESSING", bg=ACCENT,
                                   activebackground="#3a70d6")
        else:
            self.toggle_btn.config(text="PAUSE PROCESSING", bg=PANEL,
                                   activebackground="#2e323c")
        self.mode_lbl.config(
            text="target: %s" % names.get(mode, "nothing running"))
        if snap.get("daemon_pid") is not None:
            g, f = snap.get("daemon_gain"), snap.get("daemon_frames")
            txt = "daemon up (pid %s)" % snap["daemon_pid"]
            if g is not None:
                txt += " - gain %.2f, %d frames" % (g, f or 0)
                pending = self._pending_push
                stale_push = pending and (time.time() - pending[1] > 2.0
                                          or abs(pending[0] - g) < 0.001)
                if stale_push:
                    self._pending_push = None
                if not self._scale_dragging and not self._pending_push:
                    self._suppress_scale = round(g, 3)
                    self.var.set(g)
                    self.gain_val.config(text="%.2f" % g)
            if snap.get("daemon_err"):
                txt += " [busy]"
            self.status.config(text=txt, fg=MUTED)
        elif snap.get("screen_pid") is not None:
            self.status.config(text="screen overlay up (pid %s)" %
                               snap["screen_pid"], fg=MUTED)
        else:
            self.status.config(text="daemon down", fg=RED)
        self.win.after(STATUS_MS, self._refresh)

    # ------------------------------------------------------------ show/hide
    def toggle(self):
        if self.win.winfo_viewable():
            self.hide()
        else:
            self.show()

    def show(self):
        self._center()
        self.win.deiconify()
        self._noactivate()
        self.win.lift()
        snap = self.ctl.snapshot()
        mode = snap.get("active_mode")
        if self.ap_var.get() and mode in ("dx9", "dx11", "dx12", "vulkan") \
                and not gamelaunch_mode_paused(mode):
            self.ctl.submit(self.ctl.processing_toggle)
            self.on_log("[overlay] auto-paused (%s)" % mode)
        # owner request: the game FREEZES on one frame while the panel is
        # open (knobs turn calmly); closing the panel resumes it
        if self.ctl.cfg.get("overlay_freeze") and mode in (
                "dx9", "dx11", "dx12", "vulkan"):
            pid = self.ctl.active_game_pid()
            if pid:
                ok, msg = suspend_pid(pid)
                self._frozen_pid = pid if ok else None
                self.on_log("[overlay] %s" % msg)
        self.win.after(STATUS_MS, self._refresh)

    def unfreeze(self):
        """Resume a frozen game; called on hide AND on manager close."""
        if self._frozen_pid is not None:
            ok, msg = resume_pid(self._frozen_pid)
            self.on_log("[overlay] %s" % msg)
            self._frozen_pid = None

    def _center(self):
        sw = self.win.winfo_screenwidth()
        sh = self.win.winfo_screenheight()
        x = max(0, (sw - self.WIDTH) // 2)
        y = max(0, (sh - self.HEIGHT) // 3)
        self.win.geometry("%dx%d+%d+%d" % (self.WIDTH, self.HEIGHT, x, y))

    def hide(self):
        self.unfreeze()
        self.win.withdraw()


# Backwards compatibility for older imports/tests.
GainKnob = ControlOverlay
