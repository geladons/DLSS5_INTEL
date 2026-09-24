# ============================================================================
# m13.processes - detached process control for daemon/overlay binaries.
#
# HARD RULES honored here (from AGENTS.md / handoffs):
#  - GPU/daemon binaries run DETACHED only (DETACHED_PROCESS | NEW_PROCESS_
#    GROUP, stdout to a log file) - a foreground GPU hang wedges the caller.
#  - Sunshine is never touched; we only manage m11d.exe / m8blive.exe.
#  - Never spawn a second m11d: pid() check BEFORE start.
# ============================================================================
import subprocess

DETACHED = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP


def find_pid(exe_name):
    """PID of the first running instance, or None. tasklist-based."""
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % exe_name],
                             capture_output=True, text=True, timeout=15).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].lower() == exe_name.lower():
            try:
                return int(parts[1])
            except ValueError:
                return None
    return None


def start_detached(exe, args, cwd, log_path):
    """Launch detached; stdout/stderr appended to log_path. Returns Popen."""
    lf = open(log_path, "a", buffering=1)
    return subprocess.Popen([exe] + list(args), cwd=cwd, stdout=lf,
                            stderr=subprocess.STDOUT, close_fds=True,
                            creationflags=DETACHED)


def stop_pid(pid, force=True):
    """taskkill by PID (works for detached children we no longer own)."""
    cmd = ["taskkill", "/PID", str(pid)]
    if force:
        cmd.append("/F")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


class ManagedProcess:
    """One managed binary (m11d or m8blive): is it up, start it, stop it."""

    def __init__(self, exe_name, exe_path, cwd, log_path, extra_args=()):
        self.exe_name = exe_name
        self.exe_path = exe_path
        self.cwd = cwd
        self.log_path = log_path
        self.extra_args = list(extra_args)

    @property
    def pid(self):
        return find_pid(self.exe_name)

    @property
    def running(self):
        return self.pid is not None

    def start(self, args=()):
        if self.running:
            return False, "already running (pid %d)" % self.pid
        start_detached(self.exe_path, self.extra_args + list(args),
                       self.cwd, self.log_path)
        return True, "started %s (log: %s)" % (self.exe_name, self.log_path)

    def stop(self):
        pid = self.pid
        if pid is None:
            return False, "%s not running" % self.exe_name
        ok, msg = stop_pid(pid)
        return ok, msg
