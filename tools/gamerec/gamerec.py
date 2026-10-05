#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# gamerec - game test recording + flicker/exposure analysis harness.
#
#   record   : ddarec.exe (DDA) -> ffmpeg h264_qsv -> mp4 at fixed fps.
#              Captures the REAL presented image (DDA sees Vulkan flip-model
#              and exclusive paths that GDI screenshots miss).
#   analyze  : decode an mp4, emit per-frame luma mean / frame-diff CSV and
#              a console summary (outlier frames, breathing amplitude).
#
# Usage:
#   gamerec.py record --sec 15 --out menu.mp4 --window GTAIV.exe
#   gamerec.py record --sec 15 --out desk.mp4            (full desktop)
#   gamerec.py analyze menu.mp4
#
# Requirements: production/tools/ffmpeg.exe, dlss5/m14-ddarec build output.
# ============================================================================
import argparse
import ctypes
import ctypes.wintypes
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DDAREC = os.path.join(ROOT, "dlss5", "m14-ddarec", "build", "Release", "ddarec.exe")
FFMPEG_CANDIDATES = [
    r"C:\Users\<owner>\Desktop\production\tools\ffmpeg.exe",
    os.path.join(ROOT, "production", "tools", "ffmpeg.exe"),
    "ffmpeg",
]


def find_ffmpeg():
    for p in FFMPEG_CANDIDATES:
        if os.path.exists(p):
            return p
    return "ffmpeg"


def window_rect(proc_name):
    """(x, y, w, h) client-area-ish rect of proc's main window via user32."""
    user32 = ctypes.windll.user32
    pid_of = ctypes.windll.kernel32.GetProcessId
    # enumerate windows, find one whose process image name matches
    EnumWindows = user32.EnumWindows
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    IsWindowVisible = user32.IsWindowVisible
    GetWindowRect = user32.GetWindowRect
    OpenProcess = ctypes.windll.kernel32.OpenProcess
    CloseHandle = ctypes.windll.kernel32.CloseHandle
    QueryFullProcessImageNameW = ctypes.windll.kernel32.QueryFullProcessImageNameW
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        if not IsWindowVisible(hwnd):
            return True
        pid = ctypes.c_ulong()
        GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = OpenProcess(0x1000, False, pid.value)  # QUERY_LIMITED_INFORMATION
        if h:
            buf = ctypes.create_unicode_buffer(512)
            sz = ctypes.c_ulong(512)
            if QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(sz)):
                if os.path.basename(buf.value).lower() == proc_name.lower():
                    r = ctypes.wintypes.RECT()
                    GetWindowRect(hwnd, ctypes.byref(r))
                    found.append((r.left, r.top, r.right - r.left, r.bottom - r.top))
            CloseHandle(h)
        return True

    ctypes.windll.user32.EnumWindows(cb, 0)
    return found[0] if found else None


def cmd_record(a):
    if not os.path.exists(DDAREC):
        sys.exit(f"ddarec.exe not built: {DDAREC}\nrun dlss5\\m14-ddarec\\build.cmd")
    ff = find_ffmpeg()

    u32 = ctypes.windll.user32
    desk_w, desk_h = u32.GetSystemMetrics(0), u32.GetSystemMetrics(1)
    region = a.region
    if a.window:
        rc = window_rect(a.window)
        if not rc:
            sys.exit(f"window for {a.window} not found (is the game running?)")
        x, y, w, h = rc
        print(f"[gamerec] window {a.window} rect={rc}")
    elif region:
        x, y, w, h = (int(v) for v in region.split(","))
    else:
        x, y, w, h = 0, 0, desk_w, desk_h

    # normalize like ddarec does (negative borders, clamp, even dims)
    if x < 0:
        w += x; x = 0
    if y < 0:
        h += y; y = 0
    if x + w > desk_w:
        w = desk_w - x
    if y + h > desk_h:
        h = desk_h - y
    w &= ~1; h &= ~1
    region = f"{x},{y},{w},{h}"
    print(f"[gamerec] effective crop={region}")

    # Pipe mode glitches at high fps (ddarec fwrite splits across the pipe
    # and ffmpeg rawvideo sees a torn packet - observed at 120 fps: a short
    # packet poisons every frame after it). Default to a two-pass raw file
    # when fps > 60; --pipe forces the old path, --keep-raw keeps the .raw.
    via_file = (a.fps > 60 and not a.pipe) or a.via_raw
    raw_path = a.out + ".raw" if via_file else None

    rec_cmd = [DDAREC, str(a.sec), str(a.fps), region,
               raw_path if via_file else "-"]
    ff_cmd = [
        ff, "-y", "-hide_banner", "-loglevel", "warning",
        "-f", "rawvideo", "-pix_fmt", "bgra", "-s", f"{w}x{h}", "-r", str(a.fps),
        "-i", raw_path if via_file else "-",
        "-c:v", a.codec, "-b:v", a.bitrate, "-pix_fmt", "nv12",
        a.out,
    ]
    print("[gamerec] rec :", " ".join(rec_cmd))
    print("[gamerec] enc :", " ".join(ff_cmd))

    if via_file:
        with open(os.devnull, "wb") as dn:
            rec = subprocess.run(rec_cmd, stdout=dn, stderr=subprocess.PIPE)
        print(rec.stderr.decode("utf-8", "replace").strip())
        enc = subprocess.run(ff_cmd)
        if not a.keep_raw and os.path.exists(raw_path):
            os.remove(raw_path)
        print(f"[gamerec] wrote {a.out} rc_rec={rec.returncode} rc_enc={enc.returncode}")
        return

    rec = subprocess.Popen(rec_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    enc = subprocess.Popen(ff_cmd, stdin=rec.stdout, stderr=subprocess.STDOUT)
    rec.stdout.close()          # let enc own the pipe
    enc.wait()
    rec.wait()
    err = rec.stderr.read().decode("utf-8", "replace")
    print(err.strip())
    print(f"[gamerec] wrote {a.out} rc_rec={rec.returncode} rc_enc={enc.returncode}")


def cmd_analyze(a):
    ff = find_ffmpeg()
    # decode to gray8 rawvideo, one byte per pixel
    p = subprocess.Popen(
        [ff, "-hide_banner", "-loglevel", "error", "-i", a.video,
         "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    # probe dims
    probe = subprocess.run(
        [ff, "-hide_banner", "-i", a.video],
        capture_output=True, text=True,
    ).stderr
    import re
    m = re.search(r"(\d{2,5})x(\d{2,5})", probe)
    fps_m = re.search(r"(\d+(?:\.\d+)?) fps", probe)
    W, H = int(m.group(1)), int(m.group(2))
    fps = float(fps_m.group(1)) if fps_m else 60.0

    try:
        import numpy as np
    except ImportError:
        sys.exit("numpy required: pip install numpy")

    frame_sz = W * H
    rows = []
    prev = None
    i = 0
    while True:
        buf = p.stdout.read(frame_sz)
        if len(buf) < frame_sz:
            break
        f = np.frombuffer(buf, dtype=np.uint8)
        mean = float(f.mean())
        std = float(f.std())
        diff = float(np.abs(f.astype(np.int16) - prev).mean()) if prev is not None else 0.0
        rows.append((i, mean, std, diff))
        prev = f
        i += 1
    p.wait()

    base = os.path.splitext(a.video)[0]
    csv_path = base + ".luma.csv"
    with open(csv_path, "w") as fp:
        fp.write("frame,luma_mean,luma_std,diff\n")
        for r in rows:
            fp.write(f"{r[0]},{r[1]:.3f},{r[2]:.3f},{r[3]:.3f}\n")

    means = np.array([r[1] for r in rows])
    diffs = np.array([r[3] for r in rows[1:]])
    med = float(np.median(means))
    out_hi = int(np.sum(means > med * 1.10))
    out_lo = int(np.sum(means < med * 0.90))
    print(f"[analyze] {a.video}: {len(rows)} frames {W}x{H} @{fps:g}fps")
    print(f"  luma mean: min={means.min():.1f} med={med:.1f} max={means.max():.1f} "
          f"pp-p2p={means.max()-means.min():.1f}")
    if len(means) > 2:
        drift = np.abs(np.diff(means))
        print(f"  frame-to-frame luma delta: med={np.median(drift):.2f} "
              f"p95={np.percentile(drift,95):.2f} max={drift.max():.2f}")
    print(f"  outliers vs median: >+10%={out_hi}  <-10%={out_lo}")
    if len(diffs):
        print(f"  pixel diff (same-scene shimmer): med={np.median(diffs):.2f} "
              f"p95={np.percentile(diffs,95):.2f}")
    print(f"  wrote {csv_path}")


def main():
    ap = argparse.ArgumentParser(prog="gamerec")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record")
    r.add_argument("--sec", type=float, default=15.0)
    r.add_argument("--fps", type=float, default=60.0)
    r.add_argument("--out", required=True)
    r.add_argument("--region", default=None, help="x,y,w,h (default: full desktop)")
    r.add_argument("--window", default=None, help="exe name -> capture its window rect")
    r.add_argument("--codec", default="h264_qsv")
    r.add_argument("--bitrate", default="20M")
    r.add_argument("--via-raw", action="store_true",
                   help="two-pass record via .raw file (auto-on above 60 fps)")
    r.add_argument("--keep-raw", action="store_true")
    r.add_argument("--pipe", action="store_true",
                   help="force the stdout-pipe path even above 60 fps")
    r.set_defaults(fn=cmd_record)

    az = sub.add_parser("analyze")
    az.add_argument("video")
    az.set_defaults(fn=cmd_analyze)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
