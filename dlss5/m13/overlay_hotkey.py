# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_hotkey - global hotkey spec parsing + polling for the m13
# overlay (default CTRL+ALT+G). Edge-triggered GetAsyncKeyState polling on
# a Tk after() timer; the overlay toplevel only supplies the timer widget.
# ============================================================================
from .overlay_win32 import _u32

VK = {"control": 0x11, "ctrl": 0x11, "alt": 0x12, "shift": 0x10}
POLL_MS = 120


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
        self._tk = None

    def _down(self, vk):
        return bool(_u32.GetAsyncKeyState(vk) & 0x8000)

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
