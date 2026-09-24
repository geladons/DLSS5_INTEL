# Screenshot driver (validation only, not shipped): real ManagerUI with the
# overlay visible, holds still ~10 s while an external process grabs the
# screen, then writes a marker and exits.
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from m13.ui import ManagerUI

ui = ManagerUI()
ui.overlay.show()
end = time.time() + 10.0
while time.time() < end:
    ui.root.update()
    time.sleep(0.05)
ui._on_close()
print("driver done")
