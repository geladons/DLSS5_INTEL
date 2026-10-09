#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# desk_overlay.py - recording-companion overlay for DLSS5_INTEL sessions.
#
# Borderless always-on-top panels meant to be captured by a whole-desktop
# recording (OBS/Sunshine) alongside the game window. Launch a subset with
#   pythonw desk_overlay.py --panels clock,fps,load
# Single instance: a new copy kills the previous one via the pidfile, so
# the manager's apply() == respawn-with-new-panels.
#
#   CLOCK  - date + HH:MM:SS, refreshed at 5 Hz (video timecode)
#   FPS    - game present rate. Sources, first fresh wins:
#              daemon m11d_last.log  "frame N ... in X ms"  (x86 TCP path:
#                sync serving means the daemon cadence IS the game rate)
#              layer nr_layer_win.log "[diag] f=N"         (NR_LAYER_DIAG)
#              layer "fg stats: presents=N" delta          (sparse, x64)
#            eng = the daemon's last chain ms, shown only while that log
#            is live (>4 s stale -> '--', a dead session can't fake data)
#   LOAD   - CPU % (GetSystemTimes), RAM % (GlobalMemoryStatusEx),
#            GPU % (typeperf "GPU Engine" counters, busiest engine).
#            Smoothed (EMA) so the numbers read like Task Manager, not a
#            200 ms jitter stream.
#
# No third-party deps: tkinter + ctypes + a typeperf helper process.
# Drag panels with the left mouse button. Right-click or Esc closes.
# Positions persist in %TEMP%\desk_overlay_pos.json.
# ============================================================================
import ctypes
import ctypes.wintypes as wt
import csv
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk

LAYER_LOG = os.path.join(tempfile.gettempdir(), "nr_layer_win.log")
DAEMON_LOG = os.path.join(tempfile.gettempdir(), "m11d_last.log")
POS_FILE = os.path.join(tempfile.gettempdir(), "desk_overlay_pos.json")
PID_FILE = os.path.join(tempfile.gettempdir(), "desk_overlay.pid")
FRESH_S = 4.0                    # a log older than this feeds '--'

BG, FG, DIM = "#0a0e0a", "#9dff57", "#5c7a4a"
FONT_VAL = ("Consolas", 18, "bold")
FONT_LBL = ("Consolas", 9)

ALL_PANELS = ("clock", "fps", "load")

# ---------------------------------------------------------------- counters


class _FILETIME(ctypes.Structure):
    _fields_ = [("dwLow", wt.DWORD), ("dwHigh", wt.DWORD)]


class _MEMSTAT(ctypes.Structure):
    _fields_ = [("dwLength", wt.DWORD), ("dwMemoryLoad", wt.DWORD),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]


def _ft_to_i64(ft):
    return (ft.dwHigh << 32) | ft.dwLow


class CpuMeter:
    """CPU busy % via GetSystemTimes deltas (no psutil dependency)."""

    def __init__(self):
        self._prev = self._read()

    def _read(self):
        idle, kern, user = _FILETIME(), _FILETIME(), _FILETIME()
        ctypes.windll.kernel32.GetSystemTimes(
            ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user))
        return (_ft_to_i64(idle), _ft_to_i64(kern), _ft_to_i64(user))

    def sample(self):
        i, k, u = self._prev
        ni, nk, nu = self._read()
        self._prev = (ni, nk, nu)
        busy = (nk - k) + (nu - u)
        total = busy + (ni - i)
        return 0.0 if total <= 0 else 100.0 * busy / total


def ram_percent():
    m = _MEMSTAT()
    m.dwLength = ctypes.sizeof(_MEMSTAT)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return float(m.dwMemoryLoad)


class GpuMeter(threading.Thread):
    """Streams `typeperf -si 1` GPU-engine counters; keeps the row max.

    The counter set spans every (pid, luid, engine) tuple, so the busiest
    engine across all GPUs is a fair "GPU load" proxy for a recording.
    """

    def __init__(self):
        super().__init__(daemon=True)
        self.value = None            # None until first sample lands
        self._proc = None
        self._stop = False

    def run(self):
        try:
            self._proc = subprocess.Popen(
                ["typeperf", "-si", "1",
                 "\\GPU Engine(*)\\Utilization Percentage"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True, bufsize=1,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            for line in self._proc.stdout:
                if self._stop:
                    break
                line = line.strip()
                if not line.startswith('"') or "(PDH" in line:
                    continue
                try:
                    row = next(csv.reader(io.StringIO(line)))
                    vals = [float(v) for v in row[1:] if v]
                    if vals:
                        self.value = max(vals)
                except (ValueError, csv.Error):
                    continue
        except OSError:
            self.value = None

    def stop(self):
        self._stop = True
        if self._proc:
            try:
                self._proc.kill()
            except OSError:
                pass


# ---------------------------------------------------------------- fps read
_F_RE = re.compile(rb"\bf=(\d+)")
_MS_RE = re.compile(rb"in ([\d.]+) ms")
_DF_RE = re.compile(rb"\bm11d\] frame (\d+)")
_PRES_RE = re.compile(rb"presents=(\d+)")


def _tail(path, nbytes=65536):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            return f.read()
    except OSError:
        return b""


def _fresh(path):
    try:
        return time.time() - os.path.getmtime(path) < FRESH_S
    except OSError:
        return False


class FpsSource:
    """Present-rate estimator with three sources, first fresh wins:

    1. daemon m11d_last.log `frame N` counter - live only while the log
       file is being written (mtime fresh). Under the x86 sync path the
       daemon's served cadence IS the displayed frame rate.
    2. layer nr_layer_win.log `[diag] f=N` counter (needs NR_LAYER_DIAG).
    3. layer `fg stats: presents=N` - written every 300 presents; sparse
       but path-agnostic, covers the x64 in-process engine.
    """

    def __init__(self):
        self._s = {}                 # source -> (counter, t)
        self._last_fps = None

    def _rate(self, key, cur):
        prev = self._s.get(key)
        self._s[key] = (cur, time.time())
        if prev is None:
            return None
        dt = time.time() - prev[1]
        if 0.15 < dt < 5.0 and cur > prev[0]:
            return (cur - prev[0]) / dt
        return None

    def sample(self):
        fps = eng = None
        if _fresh(DAEMON_LOG):
            buf = _tail(DAEMON_LOG, 32768)
            m = list(_DF_RE.finditer(buf))
            if m:
                r = self._rate("daemon", int(m[-1].group(1)))
                if r is not None:
                    fps = r
            d = list(_MS_RE.finditer(buf))
            if d:
                eng = float(d[-1].group(1))
            if fps is None:
                fps = self._last_fps          # between counter lines
        if fps is None and _fresh(LAYER_LOG):
            buf = _tail(LAYER_LOG)
            m = list(_F_RE.finditer(buf))
            if m:
                fps = self._rate("diag", int(m[-1].group(1)))
            if fps is None:
                p = list(_PRES_RE.finditer(buf))
                if p:
                    fps = self._rate("stats", int(p[-1].group(1)))
            if fps is None:
                fps = self._last_fps
        if fps is None and self._s:
            # every source quiet -> forget the last rate so '--' shows
            last_t = max(t for _c, t in self._s.values())
            if time.time() - last_t > FRESH_S:
                self._last_fps = None
        if fps is not None:
            self._last_fps = fps
        return self._last_fps, eng


# ---------------------------------------------------------------- widgets
class Panel(tk.Toplevel):
    def __init__(self, root, key, x, y, on_close):
        super().__init__(root)
        self.key = key
        self.on_close = on_close
        self.overrideredirect(True)
        self.attributes("-topmost", True, "-alpha", 0.85)
        self.configure(bg=BG)
        self.geometry("+%d+%d" % (x, y))
        self._drag = None
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._move)
        self.bind("<ButtonPress-3>", lambda e: on_close())
        self.bind("<Escape>", lambda e: on_close())

    def _press(self, e):
        self._drag = (e.x, e.y)

    def _move(self, e):
        if self._drag:
            dx, dy = e.x - self._drag[0], e.y - self._drag[1]
            self.geometry("+%d+%d" % (self.winfo_x() + dx,
                                      self.winfo_y() + dy))

    def make(self, text, font=FONT_LBL, fg=DIM):
        lbl = tk.Label(self, text=text, font=font, fg=fg, bg=BG,
                       padx=8, pady=0)
        lbl.pack(anchor="w")
        lbl.bind("<ButtonPress-1>", self._press)
        lbl.bind("<B1-Motion>", self._move)
        lbl.bind("<ButtonPress-3>", lambda e: self.on_close())
        return lbl


# ------------------------------------------------------------ single inst
def _claim_instance():
    """Kill a previous overlay (pidfile) and take over the slot."""
    try:
        with open(PID_FILE) as f:
            old = int(f.read().strip())
    except (OSError, ValueError):
        old = 0
    if old and old != os.getpid():
        h = ctypes.windll.kernel32.OpenProcess(0x1001, False, old)
        if h:                               # QUERY_LIM | TERMINATE
            buf = ctypes.create_unicode_buffer(260)
            size = ctypes.c_ulong(260)
            is_py = ctypes.windll.kernel32.QueryFullProcessImageNameW(
                h, 0, buf, ctypes.byref(size)) \
                and "python" in buf.value.lower()
            if is_py:
                ctypes.windll.kernel32.TerminateProcess(h, 0)
            ctypes.windll.kernel32.CloseHandle(h)
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))


def main():
    import argparse
    ap = argparse.ArgumentParser(prog="desk_overlay")
    ap.add_argument("--panels", default=",".join(ALL_PANELS),
                    help="comma list: clock,fps,load")
    args = ap.parse_args()
    want = [p for p in args.panels.split(",") if p in ALL_PANELS]
    if not want:
        return 0
    _claim_instance()

    root = tk.Tk()
    root.withdraw()
    try:
        pos = json.load(open(POS_FILE))
    except (OSError, ValueError):
        pos = {}
    panels = {}

    def on_close():
        p = {k: (w.winfo_x(), w.winfo_y()) for k, w in panels.items()}
        try:
            json.dump(p, open(POS_FILE, "w"))
        except OSError:
            pass
        gpu.stop()
        try:
            os.unlink(PID_FILE)
        except OSError:
            pass
        root.destroy()

    def spawn(key, dx, dy):
        x, y = pos.get(key, (dx, dy))
        panels[key] = Panel(root, key, x, y, on_close)
        return panels[key]

    gpu = GpuMeter()
    cpu = CpuMeter()
    fpssrc = FpsSource()
    clock_lbl = fps_lbl = load_lbl = None

    if "clock" in want:
        w = spawn("clock", 30, 30)
        w.make("DATE / TIME")
        clock_lbl = w.make("--", FONT_VAL, "#ffffff")
    if "fps" in want:
        w = spawn("fps", 30, 120)
        w.make("GAME PRESENT / ENGINE")
        fps_lbl = w.make("--", FONT_VAL, FG)
    if "load" in want:
        w = spawn("load", 30, 210)
        w.make("CPU / RAM / GPU")
        load_lbl = w.make("--", FONT_VAL, "#66ccff")
        gpu.start()

    ema = {}                                 # EMA-smoothed load values

    def smooth(k, v):
        ema[k] = v if k not in ema else ema[k] * 0.65 + v * 0.35
        return ema[k]

    def tick():
        if clock_lbl is not None:
            clock_lbl.config(text=time.strftime("%d.%m.%Y %H:%M:%S"))
        if fps_lbl is not None:
            fps, eng = fpssrc.sample()
            fps_lbl.config(
                text=("%5.1f fps" % fps if fps is not None else "  -- fps")
                     + ("  eng %.0fms" % eng if eng is not None else ""))
        if load_lbl is not None:
            g = gpu.value
            load_lbl.config(text="C %3.0f%%  R %3.0f%%  G %s%%"
                                 % (smooth("c", cpu.sample()),
                                    smooth("r", ram_percent()),
                                    "%3.0f" % smooth("g", g)
                                    if g is not None else " --"))
        root.after(400, tick)

    root.after(400, tick)
    root.mainloop()


if __name__ == "__main__":
    sys.exit(main())
