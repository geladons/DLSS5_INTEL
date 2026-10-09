#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# desk_overlay.py - recording-companion overlay for DLSS5_INTEL sessions.
#
# Three borderless always-on-top panels meant to be captured by a
# whole-desktop recording (OBS/Sunshine) alongside the game window:
#
#   CLOCK  - date + HH:MM:SS, refreshed at 5 Hz (video timecode)
#   FPS    - game present rate derived from the layer's diag counter
#            (f= delta / wall time) + the m11d engine's last frame ms
#   LOAD   - CPU % (GetSystemTimes), RAM % (GlobalMemoryStatusEx),
#            GPU % (typeperf "GPU Engine" counters, busiest engine)
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

BG, FG, DIM = "#0a0e0a", "#9dff57", "#5c7a4a"
FONT_VAL = ("Consolas", 18, "bold")
FONT_LBL = ("Consolas", 9)

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
                text=True, bufsize=1)
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


def _tail(path, nbytes=65536):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            return f.read()
    except OSError:
        return b""


class FpsSource:
    """Present rate = layer diag f= counter delta over wall time.
    Engine ms = last `in X ms` the daemon logged. A stale log (>4 s
    without growth) shows '--' so a dead session can't fake a rate."""

    def __init__(self):
        self._f = None
        self._t = None
        self._size = 0
        self._last_grow = time.time()

    def sample(self):
        now = time.time()
        try:
            size = os.path.getsize(LAYER_LOG)
        except OSError:
            size = 0
        buf = _tail(LAYER_LOG)
        m = list(_F_RE.finditer(buf))
        cur = int(m[-1].group(1)) if m else None
        if size != self._size:
            self._size = size
            self._last_grow = now
        fps = None
        if cur is not None and self._f is not None:
            dt = now - self._t
            if 0.15 < dt < 5.0 and cur > self._f:
                fps = (cur - self._f) / dt
        if cur is not None:
            self._f, self._t = cur, now
        if now - self._last_grow > 4.0:
            fps = None
        eng = None
        dbuf = _tail(DAEMON_LOG, 32768)
        d = list(_MS_RE.finditer(dbuf))
        if d:
            eng = float(d[-1].group(1))
        return fps, eng


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


def main():
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
        root.destroy()

    def spawn(key, dx, dy):
        x, y = pos.get(key, (dx, dy))
        panels[key] = Panel(root, key, x, y, on_close)
        return panels[key]

    # CLOCK --------------------------------------------------------------
    w = spawn("clock", 30, 30)
    w.make("DATE / TIME")
    clock_lbl = w.make("--", FONT_VAL, "#ffffff")

    # FPS ----------------------------------------------------------------
    w = spawn("fps", 30, 120)
    w.make("GAME PRESENT / ENGINE")
    fps_lbl = w.make("--", FONT_VAL, FG)

    # LOAD ---------------------------------------------------------------
    w = spawn("load", 30, 210)
    w.make("CPU / RAM / GPU")
    load_lbl = w.make("--", FONT_VAL, "#66ccff")

    cpu = CpuMeter()
    gpu = GpuMeter()
    gpu.start()
    fpssrc = FpsSource()

    def tick():
        now = time.localtime()
        clock_lbl.config(text=time.strftime("%d.%m.%Y %H:%M:%S", now))
        fps, eng = fpssrc.sample()
        fps_lbl.config(
            text=("%5.1f fps" % fps if fps is not None else "  -- fps")
                 + ("  eng %.0fms" % eng if eng is not None else ""))
        g = gpu.value
        load_lbl.config(text="C %3.0f%%  R %3.0f%%  G %s%%"
                             % (cpu.sample(), ram_percent(),
                                "%3.0f" % g if g is not None else " --"))
        root.after(200, tick)

    root.after(200, tick)
    root.mainloop()


if __name__ == "__main__":
    sys.exit(main())
