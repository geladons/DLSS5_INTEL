# ============================================================================
# m13.processes - process probes and detached launches.
#
# GOTCHA (owner-found 2026-09-23): pythonw has NO console, so every
# subprocess.run(["tasklist"]) spawns a VISIBLE console window that flashes
# and closes - the 1 s UI poll did this twice a second (m11d + m8blive
# probes), the screen strobed and the UI wedged. ALL process queries go
# through ctypes Toolhelp32/TerminateProcess now: no child processes, no
# consoles, microseconds per call. subprocess remains only as a fallback
# and always with CREATE_NO_WINDOW.
#
# HARD RULES: GPU/daemon binaries run DETACHED only; Sunshine is never
# touched; never spawn a second m11d (pid probe BEFORE start).
# ============================================================================
import ctypes
import subprocess
from ctypes import wintypes

DETACHED = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
CREATE_NO_WINDOW = 0x08000000

_TH32CS_SNAPPROCESS = 0x00000002
_INVALID_HANDLE = ctypes.c_void_p(-1).value
_KERNEL32 = ctypes.windll.kernel32
_KERNEL32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
_KERNEL32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_KERNEL32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def find_pids(exe_name):
    """All PIDs whose image name matches (case-insensitive), Toolhelp32."""
    want = exe_name.lower()
    snap = _KERNEL32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if snap in (None, _INVALID_HANDLE):
        return []
    pids = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = _KERNEL32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == want:
                pids.append(entry.th32ProcessID)
            ok = _KERNEL32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        _KERNEL32.CloseHandle(snap)
    return pids


def find_pid(exe_name):
    """First PID of a running instance, or None."""
    pids = find_pids(exe_name)
    return pids[0] if pids else None


def stop_pid(pid, force=True):
    """TerminateProcess by PID; taskkill only as a CREATE_NO_WINDOW fallback."""
    h = _KERNEL32.OpenProcess(0x0001, False, pid)   # PROCESS_TERMINATE
    if h:
        try:
            if _KERNEL32.TerminateProcess(h, 1):
                return True, "terminated pid %d" % pid
        finally:
            _KERNEL32.CloseHandle(h)
    r = subprocess.run(["taskkill", "/PID", str(pid)] + (["/F"] if force
                        else []), capture_output=True, text=True, timeout=15,
                       creationflags=CREATE_NO_WINDOW)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def start_detached(exe, args, cwd, log_path):
    """Launch detached; stdout/stderr appended to log_path. No console."""
    lf = open(log_path, "a", buffering=1)
    return subprocess.Popen([exe] + list(args), cwd=cwd, stdout=lf,
                            stderr=subprocess.STDOUT, close_fds=True,
                            creationflags=DETACHED)


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
        return stop_pid(pid)
