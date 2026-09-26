# Detached GTA IV launcher with the m11 layer capture env set.
# Usage: python _gta4_start.py <mode>   mode: live1 | live4 | uimask
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import os
import subprocess
import sys

GAME_DIR = r"D:\downdloads\Grand Theft Auto IV"
DUMPS = r"" + str(_REPO) + r"\dlss5\m11-layer\dumps"
LOG = os.path.join(DUMPS, "gp_cap.bin")
OUT = os.path.join(DUMPS, "gp_out.bin")

mode = sys.argv[1] if len(sys.argv) > 1 else "live1"
env = dict(os.environ)
env["NR_LAYER_CAPTURE"] = LOG
env["NR_LAYER_CAPTURE_OUT"] = OUT
if mode == "live1":
    env["NR_LAYER_LIVE"] = "1"
elif mode == "live4":
    env["NR_LAYER_LIVE"] = "4"
elif mode == "uimask":
    env["NR_LAYER_TRIGGER"] = os.path.join(DUMPS, "nr_trigger.flag")
    env["NR_LAYER_UI_MASK"] = "1"
else:
    print("unknown mode", mode)
    sys.exit(2)

lf = open(os.path.join(DUMPS, "_gta4_stdout.log"), "w")
p = subprocess.Popen(
    [os.path.join(GAME_DIR, "GTAIV.exe")],
    cwd=GAME_DIR,
    stdout=lf,
    stderr=subprocess.STDOUT,
    env=env,
    creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
    close_fds=True,
)
print("GTAIV started pid", p.pid, "mode", mode)
