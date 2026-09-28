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

# slider path (debounced -> after_idle -> worker); 0.9 avoids the benign
# "equals last synced value" suppression edge
ui._suppress_scale = None
ui.gain_var.set(0.9)
ui._gain_moved(0.9)
for _ in range(10):
    ui.root.update()
    time.sleep(0.2)
g, _ = ui.ctl.client.status()
assert abs(g - 0.9) < 1e-6, "slider push failed (gain=%s)" % g
ui.log("mgr", "slider live push verified: daemon gain %.3f" % g)

# status widgets got painted from the snapshot
for _ in range(5):
    ui.root.update()
    time.sleep(0.3)
print("DAEMON LABEL:", ui.st_daemon.cget("text"))
print("LAYERS LABEL:", ui.layers_info.cget("text"))
print("HINT:", ui.hint.cget("text"))
print("OVERLAY STATUS:", ui.overlay.status.cget("text"))

# Stage 5 controls: scale presets, frame generation, sharpness
from m13.i18n import scale_to_preset_label, preset_label_to_scale

def pump(count=6, delay=0.1):
    for _ in range(count):
        ui.root.update()
        time.sleep(delay)

ui.scale_var.set(scale_to_preset_label(0.67))
ui._scale_selected()
pump()
assert ui.ctl.cfg.get("scale") == 0.67, "scale preset not updated in cfg"
assert ui.overlay.scale_var.get() == scale_to_preset_label(0.67), "overlay scale not synced"
assert os.environ.get("DLSS5_SCALE") == "0.67", "DLSS5_SCALE env not updated"
assert os.environ.get("NR_LAYER_SCALE") == "0.67", "NR_LAYER_SCALE env not updated"
assert os.environ.get("M12_SCALE") == "0.67", "M12_SCALE env not updated"
print("SCALE PRESET TEST: PASS (0.67)")

ui.framegen_var.set(False)
ui._framegen_toggled()
pump()
assert ui.ctl.cfg.get("frame_gen") is False, "frame_gen not False in cfg"
assert ui.overlay.fg_var.get() is False, "overlay frame_gen not synced"
assert os.environ.get("DLSS5_FRAMEGEN") == "0", "DLSS5_FRAMEGEN env not 0"

ui.overlay.fg_var.set(True)
ui.overlay._save_framegen()
pump()
assert ui.ctl.cfg.get("frame_gen") is True, "overlay save frame_gen failed"
assert os.environ.get("DLSS5_FRAMEGEN") == "1", "DLSS5_FRAMEGEN env not 1"
print("FRAME GENERATION TOGGLE TEST: PASS")

ui.sharpness_var.set(0.95)
ui._sharpness_moved(0.95)
pump()
assert ui.ctl.cfg.get("sharpness") == 0.95, "sharpness not updated in cfg"
assert abs(float(ui.overlay.sharpness_var.get()) - 0.95) < 1e-4, "overlay sharpness not synced"
assert os.environ.get("DLSS5_SHARPNESS") == "0.95", "DLSS5_SHARPNESS env not updated"
print("SHARPNESS SLIDER TEST: PASS (0.95)")

# restore defaults
ui.ctl.set_scale(0.50)
ui.ctl.set_framegen(True)
ui.ctl.set_sharpness(0.8)
ui.ctl.set_gain(ui.ctl.cfg.get("gain"))
ui.overlay.hide()
ui._on_close()
print("UI SMOKE: ALL PASS")
