# UI smoke: build the whole main window, run poll/drain cycles headlessly,
# exercise the knob + gain push path against the LIVE daemon, then close.
# Run: pythonw-free (console ok): python _ui_smoke.py
import os
import sys
import time

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.ui import ManagerUI

ui = ManagerUI()
ui.log("mgr", "ui smoke start")
for _ in range(6):
    ui.root.update()
    time.sleep(0.3)

# knob: show, push a live gain through the real NRCT channel
ui.knob.show()
ui.root.update()
ui.knob.var.set(1.35)
ui.knob._push()
ui.root.update()
time.sleep(0.5)
g, frames = ui.ctl.client.status()
assert abs(g - 1.35) < 1e-6, "knob push did not reach the daemon (gain=%s)" % g
ui.log("mgr", "knob live push verified: daemon gain %.3f, %d frames" % (g, frames))

# slider path
ui.gain_var.set(0.8)
ui._gain_push()
time.sleep(0.3)
g, _ = ui.ctl.client.status()
assert abs(g - 0.8) < 1e-6, "slider push failed (gain=%s)" % g
ui.log("mgr", "slider live push verified: daemon gain %.3f" % g)

# status widgets got painted
for _ in range(3):
    ui.root.update()
    time.sleep(0.3)
print("DAEMON LABEL:", ui.st_daemon.cget("text"))
print("LAYERS LABEL:", ui.st_layers.cget("text"))
print("KNOB STATUS:", ui.knob.status.cget("text"))

# leave the daemon at the configured gain
ui.ctl.set_gain(ui.ctl.cfg.get("gain"))
ui.knob.hide()
ui._on_close()
print("UI SMOKE: ALL PASS")
