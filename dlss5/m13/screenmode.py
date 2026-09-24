# ============================================================================
# m13.screenmode - the m8b-live overlay (screen mode) lifecycle.
#
# Screen mode = fullscreen click-through overlay processing the DDA capture
# of the desktop. Owner-visible runs MUST pass --echo-free 0 (the owner
# watches through the Moonlight/Sunshine stream; WDA_EXCLUDEFROMCAPTURE
# would hide the overlay from them - DEV_STATE.md).
#
# NOTE: m8blive self-restarts its loop when hidden/shown (CTRL+ALT+X) - that
# is normal, do not fight it. Gain in screen mode is a launch argument, so
# changing it = restart the overlay (no game is attached, restart is cheap).
# ============================================================================
import os

from .processes import ManagedProcess

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
M8B_EXE = os.path.join(_REPO, "dlss5", "m8b-live", "build", "Release",
                       "m8blive.exe")
M8B_CWD = os.path.dirname(M8B_EXE)
M8B_LOG = os.path.join(_REPO, "docs", "m8b-live.log")


class ScreenMode:
    def __init__(self):
        self.proc = ManagedProcess("m8blive.exe", M8B_EXE, M8B_CWD, M8B_LOG)

    @property
    def running(self):
        return self.proc.running

    def start(self, gain=1.0, extra_args=()):
        args = ["--frames", "1000000", "--echo-free", "0", "--gain",
                "%.3f" % gain]
        args += [a for a in extra_args if a]
        return self.proc.start(args)

    def stop(self):
        return self.proc.stop()
