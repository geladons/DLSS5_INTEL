# UI smoke: build the whole main window, run poll/drain cycles, exercise the
# overlay + gain push path against the LIVE daemon (actions are async via the
# worker thread now, so we pump the loop and then verify via NRCT directly).
# Run with a console: python _ui_smoke.py
import os
import sys
import time

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.ui import ManagerUI

ui = ManagerUI()
ui.log("mgr", "ui smoke start")

deadline = time.time() + 4.0
while time.time() < deadline and not ui.ctl.snapshot():
    ui.root.update()
    time.sleep(0.2)
for _ in range(3):
    ui.root.update()
    time.sleep(0.2)
assert ui.ctl.snapshot(), "monitor produced no snapshot"
print("SNAPSHOT KEYS:", sorted(ui.ctl.snapshot().keys()))

# overlay: show, push a live gain through the real NRCT channel (async)
ui.overlay.show()
ui.root.update()
ui.overlay.var.set(1.35)
ui.overlay._push()
for _ in range(10):
    ui.root.update()
    time.sleep(0.2)
g, frames = ui.ctl.client.status()
assert abs(g - 1.35) < 1e-6, "overlay push did not reach the daemon (gain=%s)" % g
ui.log("mgr", "overlay live push verified: daemon gain %.3f, %d frames"
       % (g, frames))

# slider path (debounced -> after_idle -> worker)
ui.gain_var.set(0.8)
ui._gain_moved(0.8)
for _ in range(10):
    ui.root.update()
    time.sleep(0.2)
g, _ = ui.ctl.client.status()
assert abs(g - 0.8) < 1e-6, "slider push failed (gain=%s)" % g
ui.log("mgr", "slider live push verified: daemon gain %.3f" % g)

# status widgets got painted from the snapshot
for _ in range(5):
    ui.root.update()
    time.sleep(0.3)
print("DAEMON LABEL:", ui.st_daemon.cget("text"))
print("LAYERS LABEL:", ui.layers_info.cget("text"))
print("HINT:", ui.hint.cget("text"))
print("OVERLAY STATUS:", ui.overlay.status.cget("text"))

# leave the daemon at the configured gain
ui.ctl.set_gain(ui.ctl.cfg.get("gain"))
ui.overlay.hide()
ui._on_close()
print("UI SMOKE: ALL PASS")
