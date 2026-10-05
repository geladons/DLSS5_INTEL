# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_log - durable file log for the in-game overlay.
#
# on_log callbacks feed a UI widget nobody reads mid-game; a 2026-10 field
# report showed the in-layer path silently fall back to an external window
# with zero trace. Every overlay decision (mode chosen, arm() reason, shm
# section creation, first publish, heartbeat/consumer stalls, fallback) is
# mirrored to <bundle>/logs/overlay.log - the same directory m13.pyw uses
# for manager_err.log - so field reports keep a real trace.
#
# Thread-safe, never raises, never truncates beyond a soft size guard.
# ============================================================================
import os
import threading
import time

_lock = threading.Lock()
_path = None
_CAP_BYTES = 4 * 1024 * 1024   # rotate-once at 4 MB -> overlay.log.1


def log_path():
    """Absolute path of overlay.log (resolved once, lazily)."""
    global _path
    if _path is None:
        try:
            from . import paths
            _path = os.path.join(paths.BUNDLE, "logs", "overlay.log")
        except Exception:
            _path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "logs", "overlay.log")
        _path = os.path.abspath(_path)
    return _path


def log(msg):
    """Append one timestamped line; swallows every failure (the log must
    never break the overlay it observes)."""
    try:
        p = log_path()
        line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
        with _lock:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            try:
                if os.path.getsize(p) > _CAP_BYTES:
                    os.replace(p, p + ".1")   # keep one generation back
            except OSError:
                pass
            with open(p, "a", encoding="ascii", errors="replace") as f:
                f.write(line)
    except Exception:
        pass
