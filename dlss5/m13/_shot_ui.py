# One-shot UI screenshot: main window after the scan, saved to work/.
import os
import sys
import time

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.ui import ManagerUI

ui = ManagerUI()
deadline = time.time() + 40
while time.time() < deadline and not ui.games_tab.cards:
    ui.root.update()
    time.sleep(0.3)
for _ in range(10):
    ui.root.update()
    time.sleep(0.2)

ui.root.update_idletasks()
ui.nb.select(0)
for _ in range(5):
    ui.root.update()
    time.sleep(0.2)
x = ui.root.winfo_rootx()
y = ui.root.winfo_rooty()
w = ui.root.winfo_width()
h = ui.root.winfo_height()
from PIL import ImageGrab
img = ImageGrab.grab(bbox=(x, y, x + w, y + h))
out = os.path.join(os.path.dirname(_DLSS5), "work", "_ui_v6.png")
img.save(out)
print("saved", out, img.size)
ui.overlay.hide()
ui._on_close()
