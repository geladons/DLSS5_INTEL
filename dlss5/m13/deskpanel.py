# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# deskpanel.py - spawn/stop the recording desk-overlay (tools/desk_overlay.py)
# from the manager settings tab.
#
# The overlay is a standalone pythonw process - three borderless topmost
# panels (clock / fps / load) meant to be captured by a whole-desktop
# recording next to the game window. Single-instance by pidfile: starting
# a new copy kills the previous one, so "apply" is just "spawn again with
# the new panel set". Manager-spawned panels carry --parent-pid and die
# with the manager; panels started by DESK OVERLAY.cmd have no parent and
# persist for standalone recording.
# ============================================================================
import os
import subprocess
import sys
import tempfile

PIDFILE = os.path.join(tempfile.gettempdir(), "desk_overlay.pid")

_CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
_DETACHED = getattr(subprocess, "DETACHED_PROCESS", 0)


def script_path():
    """Resolve tools/desk_overlay.py for both layouts:
    production: <bundle>/m13/deskpanel.py -> <bundle>/tools/desk_overlay.py
    repo:       <repo>/dlss5/m13/...     -> <repo>/tools/desk_overlay.py
    """
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (1, 2):
        cand = os.path.join(here, *[".."] * up, "tools", "desk_overlay.py")
        cand = os.path.normpath(cand)
        if os.path.exists(cand):
            return cand
    return None


def running_pid():
    """Pid of a live overlay instance, or None (stale/absent pidfile)."""
    try:
        with open(PIDFILE) as f:
            pid = int(f.read().strip())
    except (OSError, ValueError):
        return None
    # pid-reuse guard: the stored pid must still map to a python process
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIM
    if not h:
        return None
    buf = ctypes.create_unicode_buffer(260)
    size = ctypes.c_ulong(260)
    ok = ctypes.windll.kernel32.QueryFullProcessImageNameW(
        h, 0, buf, ctypes.byref(size))
    ctypes.windll.kernel32.CloseHandle(h)
    if not ok or "python" not in buf.value.lower():
        return None
    return pid


def stop():
    """Terminate the running overlay (if any) via its pidfile."""
    pid = running_pid()
    if pid is None:
        return False
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x0001, False, pid)  # TERMINATE
    if h:
        ctypes.windll.kernel32.TerminateProcess(h, 0)
        ctypes.windll.kernel32.CloseHandle(h)
    try:
        os.unlink(PIDFILE)
    except OSError:
        pass
    return True


def apply(panels):
    """panels: iterable of 'clock'/'fps'/'load'. Empty -> stop."""
    panels = [p for p in panels if p in ("clock", "fps", "load")]
    if not panels:
        return stop()
    script = script_path()
    if not script:
        return False
    pyw = None
    for cand in (os.path.join(os.path.dirname(sys.executable),
                              "pythonw.exe"),
                 os.path.join(os.getenv("LOCALAPPDATA", ""),
                              "Programs", "Python", "Python312",
                              "pythonw.exe")):
        if os.path.exists(cand):
            pyw = cand
            break
    if not pyw:
        import shutil
        pyw = shutil.which("pythonw") or shutil.which("pythonw.exe")
    if not pyw:
        return False
    # a fresh spawn replaces any running copy (overlay kills its
    # predecessor via the pidfile on startup) - so apply() == respawn
    subprocess.Popen(
        [pyw, script, "--panels", ",".join(panels),
         "--parent-pid", str(os.getpid())],
        cwd=os.path.dirname(script),
        creationflags=_CREATE_NO_WINDOW | _DETACHED
        | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
        close_fds=True)
    return True
