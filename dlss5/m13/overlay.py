# ============================================================================
# m13.overlay - the hotkey-invoked gain knob: a small always-on-top window
# with a live slider. Moving the slider pushes the new gain to m11d over the
# NRCT control channel - the effect is visible IN THE GAME on the next
# processed frame, no daemon restart, no game re-capture (M13 req 3).
#
# The show/hide hotkey is POLLED via GetAsyncKeyState (edge-triggered): no
# message-only window needed, and physical presses are what the owner uses.
# Injected modifiers are unreliable on this host, so we never synthesize
# keystrokes - the owner presses the combo.
# ============================================================================
import ctypes
import tkinter as tk

VK = {"control": 0x11, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
POLL_MS = 120
DEBOUNCE_MS = 250


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


class GainKnob:
    """Always-on-top slider window; on_gain(value) does the live push."""

    def __init__(self, master, on_gain, on_log, hotkey_spec, initial=1.0):
        self.on_gain = on_gain
        self.on_log = on_log
        self.win = tk.Toplevel(master)
        self.win.title("DLSS5 gain")
        self.win.attributes("-topmost", True)
        self.win.attributes("-alpha", 0.94)
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.hide)
        self.win.withdraw()

        frm = tk.Frame(self.win, padx=10, pady=8)
        frm.pack(fill="both", expand=True)
        self.var = tk.DoubleVar(value=initial)
        self.title_lbl = tk.Label(frm, text="DLSS5 gain (live)", font=("Segoe UI", 10, "bold"))
        self.title_lbl.pack(anchor="w")
        self.scale = tk.Scale(frm, from_=0.0, to=2.0, resolution=0.05,
                              orient="horizontal", length=260,
                              variable=self.var, command=self._debounced_push)
        self.scale.pack()
        self.status = tk.Label(frm, text="daemon: ?", fg="#666")
        self.status.pack(anchor="w")
        row = tk.Frame(frm)
        row.pack(fill="x", pady=(6, 0))
        tk.Button(row, text="Push now", command=self._push).pack(side="left")
        tk.Button(row, text="Reset 1.0",
                  command=lambda: self.var.set(1.0)).pack(side="left", padx=4)
        tk.Button(row, text="Hide", command=self.hide).pack(side="right")

        self._after_id = None
        self.poller = HotkeyPoller(hotkey_spec, self.toggle)
        self.poller.attach(master)

    def _debounced_push(self, _val):
        if self._after_id:
            self.win.after_cancel(self._after_id)
        self._after_id = self.win.after(DEBOUNCE_MS, self._push)

    def _push(self):
        g = round(float(self.var.get()), 3)
        try:
            ok, msg = self.on_gain(g)
        except Exception as e:      # never let the knob kill the UI
            ok, msg = False, "gain push failed: %s" % e
        self.on_log("[knob] gain %.2f -> %s" % (g, msg))
        color = "#060" if ok else "#a00"
        self.title_lbl.config(fg=color)
        self.win.after(1500, lambda: self.title_lbl.config(fg="#000"))

    def set_status(self, text):
        try:
            self.status.config(text=text)
        except tk.TclError:
            pass

    def toggle(self):
        if self.win.winfo_viewable():
            self.hide()
        else:
            self.show()

    def show(self):
        self.win.deiconify()
        self.win.lift()

    def hide(self):
        self.win.withdraw()
