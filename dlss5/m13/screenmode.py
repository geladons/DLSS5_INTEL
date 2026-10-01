# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.screenmode - the m8b-live overlay (screen mode) lifecycle.
#
# Standalone direct DDA overlay (m8blive.exe) capturing and processing
# desktop frames directly on the GPU via Vulkan.
#
# ARCHITECTURE CLARITY:
# Screen mode is 100% standalone and does NOT use the x86 bridge daemon
# (m11d.exe) or TCP network loops. It processes frames locally on the GPU.
# Because m8blive and m11d both reserve dedicated VRAM arenas, starting screen
# mode automatically stops the x86 daemon to prevent VRAM exhaustion.
#
# Owner-visible runs pass --echo-free 0 (owner watches through Moonlight/
# Sunshine stream; WDA_EXCLUDEFROMCAPTURE would hide the overlay from them).
# ============================================================================
import os

from . import paths
from .phase30 import write_phase30_knobs
from .processes import ManagedProcess

M8B_EXE = paths.find("m8b_exe")
M8B_CWD = os.path.dirname(M8B_EXE)
M8B_LOG = os.path.join(paths.find("logs"), "m8b-live.log")
KNOB_FILE = os.path.join(os.environ.get("TEMP", "."), "m13_screen_knobs.txt")


def write_knobs(gain, blend, scale=None, framegen=None, phase30=None):
    """Live retune of the running m8blive (polled per processed frame)."""
    try:
        with open(KNOB_FILE, "w") as f:
            parts = ["%.4f %.4f" % (gain, blend)]
            if scale is not None:
                parts.append("%.4f" % float(scale))
            if framegen is not None:
                parts.append("%d" % (1 if framegen else 0))
            f.write(" ".join(parts))
    except OSError:
        pass
    p = {"intensity": gain}
    if phase30:
        p.update(phase30)
    # Engine knobs for the live file: the in-process layer/proxy polls it
    # (a running child's env is static), so carry the current values.
    p["gain"] = float(gain)
    p["blend"] = float(blend)
    if scale is not None:
        p["scale"] = float(scale)
    if framegen is not None:
        p["frame_gen"] = 1 if framegen else 0
    write_phase30_knobs(p)


class ScreenMode:
    def __init__(self):
        self.proc = ManagedProcess("m8blive.exe", M8B_EXE, M8B_CWD, M8B_LOG)

    @property
    def running(self):
        return self.proc.running

    def start(self, gain=1.0, blend=1.0, scale=None, framegen=None, extra_args=()):
        """Fullscreen desktop overlay. NOTE: at 2560x1440 the chain is SLOW
        (weights upload ~30 s, frame 0 ~60-70 s - it is warming up, not
        dead). The UI says so; keep it honest."""
        write_knobs(gain, blend, scale, framegen)
        args = ["--frames", "1000000", "--echo-free", "0", "--gain",
                "%.3f" % gain, "--blend", "%.3f" % blend]
        if scale is not None:
            args += ["--scale", "%.2f" % float(scale)]
        args += [a for a in extra_args if a]
        return self.proc.start(args)

    def start_window(self, title, gain=1.0, blend=1.0, scale=None, framegen=None, extra_args=()):
        """Window-attached overlay (--window): much smaller extent, so much
        faster than fullscreen - the usable demo path for screen mode."""
        if not title.strip():
            return False, "window title is empty"
        write_knobs(gain, blend, scale, framegen)
        args = ["--frames", "1000000", "--echo-free", "0", "--gain",
                "%.3f" % gain, "--blend", "%.3f" % blend,
                "--window", title.strip()]
        if scale is not None:
            args += ["--scale", "%.2f" % float(scale)]
        args += [a for a in extra_args if a]
        return self.proc.start(args)

    def stop(self):
        return self.proc.stop()
