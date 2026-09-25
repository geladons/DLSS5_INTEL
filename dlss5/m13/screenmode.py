# ============================================================================
# m13.screenmode - the m8b-live overlay (screen mode) lifecycle.
#
# Screen mode = fullscreen click-through overlay processing the DDA capture
# of the desktop. Owner-visible runs MUST pass --echo-free 0 (the owner
# watches through the Moonlight/Sunshine stream; WDA_EXCLUDEFROMCAPTURE
# would hide the overlay from them - DEV_STATE.md).
#
# NOTE: m8blive self-restarts its loop when hidden/shown (CTRL+ALT+X) - that
# is normal, do not fight it. Gain/blend in screen mode are LIVE: m8blive
# polls KNOB_FILE once per processed frame, so the manager retunes it by
# rewriting the file - NO restart (a restart costs the ~30 s weights upload).
# ============================================================================
import os

from . import paths
from .processes import ManagedProcess

M8B_EXE = paths.find("m8b_exe")
M8B_CWD = os.path.dirname(M8B_EXE)
M8B_LOG = os.path.join(paths.find("logs"), "m8b-live.log")
KNOB_FILE = os.path.join(os.environ.get("TEMP", "."), "m13_screen_knobs.txt")


def write_knobs(gain, blend):
    """Live retune of the running m8blive (polled per processed frame)."""
    try:
        with open(KNOB_FILE, "w") as f:
            f.write("%.4f %.4f" % (gain, blend))
    except OSError:
        pass


class ScreenMode:
    def __init__(self):
        self.proc = ManagedProcess("m8blive.exe", M8B_EXE, M8B_CWD, M8B_LOG)

    @property
    def running(self):
        return self.proc.running

    def start(self, gain=1.0, blend=1.0, extra_args=()):
        """Fullscreen desktop overlay. NOTE: at 2560x1440 the chain is SLOW
        (weights upload ~30 s, frame 0 ~60-70 s - it is warming up, not
        dead). The UI says so; keep it honest."""
        write_knobs(gain, blend)
        args = ["--frames", "1000000", "--echo-free", "0", "--gain",
                "%.3f" % gain, "--blend", "%.3f" % blend]
        args += [a for a in extra_args if a]
        return self.proc.start(args)

    def start_window(self, title, gain=1.0, blend=1.0, extra_args=()):
        """Window-attached overlay (--window): much smaller extent, so much
        faster than fullscreen - the usable demo path for screen mode."""
        if not title.strip():
            return False, "window title is empty"
        write_knobs(gain, blend)
        args = ["--frames", "1000000", "--echo-free", "0", "--gain",
                "%.3f" % gain, "--blend", "%.3f" % blend,
                "--window", title.strip()]
        args += [a for a in extra_args if a]
        return self.proc.start(args)

    def stop(self):
        return self.proc.stop()
