# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_hotkey - global hotkey spec parsing + polling for the m13
# overlay (default CTRL+ALT+G). Edge-triggered GetAsyncKeyState polling on
# a dedicated daemon thread; the fire is marshalled onto Tk via after(0).
# (Was a Tk after() chain: any multi-second mainloop stall - observed
# 2.2-18.1 s under foreign daemon-restart churn - blinded the poller and
# swallowed whole CTRL+ALT+G presses, so the overlay could not be closed
# exactly while it was strobing.)
# ============================================================================
import threading
import time

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
    """Edge-triggered global combo polling, thread-driven."""

    def __init__(self, spec, on_fire, poll_ms=POLL_MS):
        self.mods, self.vk = parse_hotkey(spec)
        self.on_fire = on_fire
        self.poll_ms = poll_ms
        self._armed = True
        self._tk = None
        self._stop = False
        self._th = None

    def _down(self, vk):
        return bool(_u32.GetAsyncKeyState(vk) & 0x8000)

    def _combo(self):
        combo = self._down(self.vk)
        if combo and self.mods:
            combo = all(self._down(vk) for bit, vk in
                        ((0x0002, VK["control"]), (0x0001, VK["alt"]),
                         (0x0004, VK["shift"])) if self.mods & bit)
        return combo

    def _fire(self):
        # Tk thread (via after(0)). A stall only delays it - the edge was
        # already consumed by the polling thread, never lost.
        try:
            self.on_fire()
        except Exception:
            pass

    def _loop(self):
        while not self._stop:
            try:
                if self._combo():
                    if self._armed:
                        self._armed = False
                        tk = self._tk
                        if tk is not None:
                            try:
                                tk.after(0, self._fire)
                            except Exception:
                                pass
                else:
                    self._armed = True
            except Exception:
                pass
            time.sleep(self.poll_ms / 1000.0)

    def poll(self):
        """Legacy entry: kept so an old attached after() chain dying is
        harmless; the thread does the real work."""
        return

    def attach(self, tk_widget):
        self._tk = tk_widget
        if self._th is None or not self._th.is_alive():
            self._stop = False
            self._th = threading.Thread(target=self._loop, daemon=True,
                                        name="m13-hotkey")
            self._th.start()
