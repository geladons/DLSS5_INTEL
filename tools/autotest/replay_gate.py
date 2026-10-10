#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# replay_gate.py - daemon temporal-behavior release gate.
#
# Feeds a raw BGRA frame sequence to a RUNNING m11d daemon over the TCP
# frame protocol (NRN0 magic, same wire format the x86 layer uses) and
# scores the returned frames against the input itself. Screen-capture
# analysis cannot see this: conditioned metrics need the INPUT frames.
#
# Detected artifact classes (all measured on the real GTA IV flythrough):
#   1. transition-flash  - a single-frame whole-frame luma step the input
#      did not make (the 2026-10-09 "+6.59 in one frame" gate-toggle pop).
#   2. still-input flicker - output byte changes on pixels whose input
#      byte did not change (latch release churn / raw-pred jitter).
#   3. frozen-stale      - output pixels pinned while the input under
#      them moved (the slow-pan phantom mask).
#   4. opposite-sign luma swings - global breathing (brighten-then-dim
#      reversals the input never made).
#
# Usage:
#   python replay_gate.py <sequence.raw|manifest.txt> [--w 800 --h 600]
#                         [--port 48990] [--expect-pass]
# sequence.raw = rawvideo BGRA frames concatenated, WxH fixed.
# manifest.txt = lines "RAW <path>" / "PAN <dx> <dy>" / "HOLD <n>" -
# builds a sequence from one base frame (synthetic fixtures without an
# external capture).
#
# Exit code 0 = PASS, 1 = FAIL, 2 = usage/transport error.
# ============================================================================
import argparse
import os
import socket
import struct
import sys
import time

import numpy as np

MAGIC = 0x304E524E
FMT_BGRA = 44


def recv_exact(sock, n):
    buf = bytearray(n)
    mv = memoryview(buf)
    got = 0
    while got < n:
        k = sock.recv_into(mv[got:], n - got)
        if not k:
            raise OSError("connection closed mid-frame")
        got += k
    return buf


class Daemon:
    def __init__(self, port, w, h):
        self.port, self.w, self.h = port, w, h
        self.sock = None

    def _connect(self, tries=240):
        for _ in range(tries):
            try:
                self.sock = socket.create_connection(
                    ("127.0.0.1", self.port), timeout=5)
                self.sock.settimeout(300)
                return
            except OSError:
                time.sleep(0.5)
        raise OSError("daemon not reachable on %d" % self.port)

    def frame(self, fr):
        hdr = struct.pack("<4I", MAGIC, self.w, self.h, FMT_BGRA)
        for attempt in range(30):
            try:
                if self.sock is None:
                    self._connect()
                self.sock.sendall(hdr + fr.tobytes())
                out = recv_exact(self.sock, self.w * self.h * 4)
                return np.frombuffer(out, dtype=np.uint8
                                     ).reshape(self.h, self.w, 4).copy()
            except OSError:
                self.sock = None
                time.sleep(1.0)
        raise OSError("daemon unreachable after retries")


def load_sequence(path, w, h):
    """Returns N,H,W,4 uint8 array."""
    px = w * h * 4
    if path.endswith(".txt"):
        base = None
        seq = []
        for ln in open(path):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            tok = ln.split()
            if tok[0] == "RAW":
                base = np.fromfile(tok[1], dtype=np.uint8)
                base = base[:px].reshape(h, w, 4).copy()
                seq.append(base)
            elif tok[0] == "PAN":
                dx, dy = int(tok[1]), int(tok[2])
                out = base.copy()
                src = base
                if dx > 0:
                    out[:, dx:, :] = src[:, :w - dx, :]
                elif dx < 0:
                    out[:, :w + dx, :] = src[:, -dx:, :]
                if dy > 0:
                    out[dy:, :, :] = out[:h - dy, :, :]
                elif dy < 0:
                    out[:h + dy, :, :] = out[-dy:, :, :]
                base = out
                seq.append(out)
            elif tok[0] == "HOLD":
                seq.extend([base] * int(tok[1]))
        arr = np.stack(seq)
        return arr
    raw = np.fromfile(path, dtype=np.uint8)
    n = raw.size // px
    if n == 0:
        raise ValueError("no whole frames in %s" % path)
    return raw[:n * px].reshape(n, h, w, 4)


def analyze(seq, outs, fps_hint):
    """seq/outs: N,H,W,4 uint8. Returns (verdict, notes)."""
    i3 = seq[:, :, :, :3].astype(np.int16)
    o3 = outs[:, :, :, :3].astype(np.int16)
    din = np.abs(i3[1:] - i3[:-1]).max(3)
    dout = np.abs(o3[1:] - o3[:-1]).max(3)
    still = din <= 2

    notes = []
    verdict = "PASS"

    def fail(msg):
        nonlocal verdict
        verdict = "FAIL"
        notes.append("FAIL " + msg)

    def warn(msg):
        nonlocal verdict
        if verdict == "PASS":
            verdict = "WARN"
        notes.append("WARN " + msg)

    # ---- 1. transition flash: one-frame global luma events --------------
    li = i3.mean(axis=(1, 2, 3))
    lo = o3.mean(axis=(1, 2, 3))
    dl_in = np.diff(li)
    dl_out = np.diff(lo)
    invented = dl_out - dl_in
    # Classes: AMPLIFIED = out step exceeds the input's own step by >4/255
    # (the 2026-10-09 "+6.59 on +1.74 in" gate-toggle flash - FAIL; the
    # slewed emit can still land up to its 4/255 cap in one frame, so
    # 2.5..4.0 is WARN). COUNTER = output steps opposite to the input by
    # >2/255 (FAIL >4, else WARN). LAG = a real >3/255 input step lands
    # >3/255 short this frame - the slew spreading a real move (WARN).
    amp, counter, lag, soft = [], [], [], []
    for i in range(len(invented)):
        if abs(invented[i]) <= 2.5:
            continue
        si, so = np.sign(dl_in[i]), np.sign(dl_out[i])
        weak = abs(invented[i]) <= 4.0
        if so != 0 and si != 0 and so != si and abs(dl_out[i]) > 2.0:
            (soft if weak else counter).append(i)
        elif so == si and abs(dl_out[i]) - abs(dl_in[i]) > 3.0:
            (soft if weak else amp).append(i)
        elif abs(dl_in[i]) > 3.0 and abs(dl_out[i]) < abs(dl_in[i]):
            lag.append(i)
        else:
            (soft if weak else amp).append(i)
    if amp or counter:
        fail("transition-flash: amplified %s | counter %s" % (
             ", ".join("f%d:%+.1f" % (i + 1, invented[i]) for i in amp[:6]),
             ", ".join("f%d:%+.1f" % (i + 1, invented[i])
                        for i in counter[:6])))
    if soft:
        warn("soft transition steps: %s" % ", ".join(
             "f%d:%+.1f" % (i + 1, invented[i]) for i in soft[:6]))
    if lag:
        warn("landing lag x%d (real step spread over frames): %s" % (
             len(lag), ", ".join("f%d:%+.1f" % (i + 1, invented[i])
                                 for i in lag[:6])))
    if not (amp or counter or lag) and (np.abs(invented) > 2.0).any():
        warn("invented luma step %.1f (borderline)"
             % float(np.abs(invented).max()))

    # ---- 2. still-input flicker -------------------------------------
    den = np.maximum(still.sum((1, 2)), 1)
    flk4 = ((dout > 4) & still).sum((1, 2)) / den
    flk8 = ((dout > 8) & still).sum((1, 2)) / den
    p90_4, p90_8 = np.percentile(flk4, 90), np.percentile(flk8, 90)
    mx8 = flk8.max()
    notes.append("still-px flicker>4: med %.3f p90 %.3f | >8: p90 %.3f "
                 "max %.3f" % (np.median(flk4), p90_4, p90_8, mx8))
    if p90_8 > 0.05 or mx8 > 0.20:
        fail("still-input flicker: p90 %.1f%% max %.1f%% of still px "
             "moved >8/255 in one frame" % (p90_8 * 100, mx8 * 100))
    elif p90_8 > 0.02 or mx8 > 0.10:
        warn("still-input flicker p90 %.1f%% max %.1f%%"
             % (p90_8 * 100, mx8 * 100))

    # ---- 3. frozen-stale (phantom mask) ------------------------------
    K = min(8, len(outs) // 4)
    stale = (np.abs(o3[K:] - o3[:-K]).max(3) <= 1)
    moved = (np.abs(i3[K:] - i3[:-K]).max(3) > 8)
    fr = (stale & moved).mean((1, 2))
    mx = fr.max() if fr.size else 0.0
    notes.append("frozen-stale: p95 %.3f max %.3f (window %d)"
                 % (np.percentile(fr, 95), mx, K))
    if mx > 0.02:
        fail("frozen-stale %.1f%% of moved px pinned for %d frames"
             % (mx * 100, K))
    elif mx > 0.01:
        warn("frozen-stale %.1f%%" % (mx * 100))

    # ---- 4. opposite-sign luma swings (breathing) --------------------
    swings = ((np.sign(dl_out) != np.sign(dl_in))
              & (np.abs(dl_out) > 1.0)).sum()
    osc = invented.std()
    notes.append("invented-osci std %.2f, opposite swings %d" % (osc, swings))
    if osc > 1.5 or swings > 30:
        fail("global breathing: invented-osci %.2f swings %d"
             % (osc, swings))
    elif osc > 1.0 or swings > 15:
        warn("global breathing: invented-osci %.2f swings %d"
             % (osc, swings))

    return verdict, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sequence")
    ap.add_argument("--w", type=int, default=800)
    ap.add_argument("--h", type=int, default=600)
    ap.add_argument("--port", type=int, default=48990)
    ap.add_argument("--save-out")
    args = ap.parse_args()

    seq = load_sequence(args.sequence, args.w, args.h)
    print("[replay_gate] %d input frames %dx%d -> daemon :%d"
          % (len(seq), args.w, args.h, args.port))
    d = Daemon(args.port, args.w, args.h)
    outs = np.empty_like(seq)
    t0 = time.time()
    for i, fr in enumerate(seq):
        outs[i] = d.frame(fr)
        if i % 60 == 0:
            print("  frame %d (%.1f fps)" % (i, (i + 1)
                  / max(time.time() - t0, 0.01)), flush=True)
    if args.save_out:
        np.save(args.save_out, outs)
    verdict, notes = analyze(seq, outs, 25.0)
    print("[replay_gate] verdict: %s" % verdict)
    for n in notes:
        print("  " + n)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
