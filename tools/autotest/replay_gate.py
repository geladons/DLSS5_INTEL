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
#   6. flow flicker - output vs the motion-warped previous output where
#      the input itself is exactly predicted by the block flow (boiling
#      detail / screen-anchored residual during camera motion).
#   5. ghost residue (fixture:ghost) - held output on pixels whose input
#      went black after text slid/faded away (2026-10-10 loading-word
#      phantom copies; the unconditioned hold latch pinned it forever).
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


# Frames per accumulation window in analyze(): pairwise metric slices get
# a 1-frame overlap, frozen-stale slices a K-frame overlap, so every
# per-pair scalar is identical to the old whole-array computation while
# peak memory stays ~a few hundred MB regardless of sequence length.
WIN = 150


def _udiff_max(frames, k=1):
    """|frames[k:] - frames[:-k]|.max(3) on uint8 via max-min instead of an
    int16 cast: for uint8 operands max(a,b)-min(a,b) is exactly |a-b|
    (no wraparound), so the result is value-identical to the old
    np.abs(i3[k:] - i3[:-k]).max(3) at half the memory."""
    hi = np.maximum(frames[k:], frames[:-k])
    hi -= np.minimum(frames[k:], frames[:-k])
    return hi.max(3)


def analyze(seq, outs, fps_hint):
    """seq/outs: N,H,W,4 uint8. Returns (verdict, notes)."""
    n = seq.shape[0]
    still_size = seq.shape[1] * seq.shape[2]

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

    # ---- windowed accumulation -----------------------------------------
    # li/lo: per-frame luma means. All pairwise metrics index the SECOND
    # frame (pair p-1 -> p): still share (den), still-px flicker counters
    # (n4/n8), >16px-from-motion counters (fden/fn4), and the per-pair
    # flow-flicker values appended in pair order from window slices that
    # carry a 1-frame overlap. Collecting scalars instead of whole
    # sequence int16 casts keeps peak memory ~1 GB at any length.
    li = np.empty(n)
    lo = np.empty(n)
    for a in range(0, n, WIN):
        b = min(a + WIN, n)
        li[a:b] = seq[a:b, :, :, :3].mean(axis=(1, 2, 3))
        lo[a:b] = outs[a:b, :, :, :3].mean(axis=(1, 2, 3))
    den = np.zeros(n - 1)
    n4 = np.zeros(n - 1)
    n8 = np.zeros(n - 1)
    fden = np.zeros(n - 1)
    fn4 = np.zeros(n - 1)
    ffk_res = []
    for p0 in range(1, n, WIN):
        p1 = min(p0 + WIN, n)
        iw = seq[p0 - 1:p1, :, :, :3]
        ow = outs[p0 - 1:p1, :, :, :3]
        din = _udiff_max(iw)
        dout = _udiff_max(ow)
        still = din <= 2
        s = slice(p0 - 1, p1 - 1)
        den[s] = still.sum((1, 2))
        n4[s] = ((dout > 4) & still).sum((1, 2))
        n8[s] = ((dout > 8) & still).sum((1, 2))
        mv = din > 6
        near = mv.copy()
        for dy in range(-16, 17, 8):
            for dx in range(-16, 17, 8):
                near |= np.roll(np.roll(mv, dy, 1), dx, 2)
        far = still & ~near
        fden[s] = far.sum((1, 2))
        fn4[s] = ((dout > 4) & far).sum((1, 2))
        ffk_res += _flow_flicker_list(seq[p0 - 1:p1], outs[p0 - 1:p1])

    # ---- 1. transition flash: one-frame global luma events --------------
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
    # Frames whose still share is <2% (scene cuts, whip pans) carry a
    # near-zero denominator - a handful of edge pixels then reads as a
    # 60%+ "flicker" spike. They are scored by the transition checks
    # above, not here.
    ok = den >= 0.02 * still_size
    if not ok.any():
        ok[:] = True
    den = np.maximum(den, 1)
    flk4 = (n4 / den)[ok]
    flk8 = (n8 / den)[ok]
    p90_4, p90_8 = np.percentile(flk4, 90), np.percentile(flk8, 90)
    mx8 = flk8.max()
    notes.append("still-px flicker>4 (info, incl. flat px next to motion): "
                 "med %.3f p90 %.3f | >8: p90 %.3f max %.3f"
                 % (np.median(flk4), p90_4, p90_8, mx8))
    # Gated part: still px with NO input motion within 16 px. Flat pixels
    # next to a moving edge legitimately change once the residual moves
    # with the content (motion-compensated hold), so the raw still metric
    # above over-reports on camera motion; motion is scored by flow
    # flicker below instead.
    # The gated part's motion/far masks were already accumulated in the
    # window loop above (mv = din > 6, near = the rolled dilate, far =
    # still & ~near); only the per-pair scalars survive.
    fok = fden >= 0.02 * still_size
    if not fok.any():
        fok[:] = True
    ffl = (fn4 / np.maximum(fden, 1))[fok]
    fp90 = np.percentile(ffl, 90)
    notes.append("static flicker>4 (still, >16px from motion): med %.4f "
                 "p90 %.4f max %.4f" % (np.median(ffl), fp90, ffl.max()))
    if fp90 > 0.02:
        fail("static flicker: p90 %.2f%% of static px moved >4/255"
             % (fp90 * 100))
    elif fp90 > 0.01:
        warn("static flicker p90 %.2f%%" % (fp90 * 100))

    # ---- 2b. motion-conditioned (flow) flicker -----------------------
    # Owner-visible "phantoms / noise / digital garbage on any motion":
    # v9 scored med 20.7% on the native flythrough (== temporal off,
    # 19.2%) and was rejected in the field -> it must FAIL here.
    ffk = np.array(ffk_res) if ffk_res else np.zeros(1)
    fmed, fp90w = float(np.median(ffk)), float(np.percentile(ffk, 90))
    notes.append("flow flicker>4 (output vs motion-warped prev, where the "
                 "input warp is exact): med %.3f p90 %.3f max %.3f"
                 % (fmed, fp90w, ffk.max()))
    if fmed > 0.10 or fp90w > 0.20:
        fail("motion noise: flow flicker med %.1f%% p90 %.1f%%"
             % (fmed * 100, fp90w * 100))
    elif fmed > 0.04 or fp90w > 0.08:
        warn("motion noise: flow flicker med %.1f%% p90 %.1f%%"
             % (fmed * 100, fp90w * 100))

    # ---- 3. frozen-stale (phantom mask) ------------------------------
    K = min(8, len(outs) // 4)
    # Same K-lag per-frame-pair metric, accumulated over WIN-entry windows
    # whose slices reach K frames back (identical stale/moved pairs).
    fr_parts = []
    for t0 in range(K, n, WIN):
        t1 = min(t0 + WIN, n)
        iw = seq[t0 - K:t1, :, :, :3]
        ow = outs[t0 - K:t1, :, :, :3]
        stale = _udiff_max(ow, K) <= 1
        moved = _udiff_max(iw, K) > 8
        fr_parts.append((stale & moved).mean((1, 2)))
    fr = np.concatenate(fr_parts) if fr_parts else np.zeros(0)
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


def flow_flicker(seq, outs, search=8, blk=8):
    """Motion-conditioned flicker: the output must move the way the input
    moves. Per 16x16 block an integer flow is block-matched on half-res
    input luma (+-2*search full-res px, zero-motion bias); pixels whose
    INPUT is predicted by that flow within 2/255 must also have their
    OUTPUT predicted within 4/255. Zero flow reduces to the still-input
    metric; on pans it scores detail that boils or stays screen-anchored
    while the content under it moves (2026-10-10 owner report: "phantoms
    and digital garbage on any motion" - invisible to the still metric).
    Returns per-frame fractions (frames with <5% valid px are skipped)."""
    res = _flow_flicker_list(seq, outs, search, blk)
    return np.array(res) if res else np.zeros(1)


def _flow_flicker_list(seq, outs, search=8, blk=8):
    """The flow_flicker per-pair values as a list, so analyze() can call
    it on 1-frame-overlapped window slices and concatenate in pair order
    (windowing changes nothing per-pair: only seq[t] and seq[t-1] are
    read inside the loop)."""
    n, h, w = seq.shape[:3]
    hh, hw = (h // 2) // blk * blk, (w // 2) // blk * blk
    m = 2 * search + 2
    res = []
    for t in range(1, n):
        a = seq[t, :2 * hh, :2 * hw, :3].astype(np.float32).mean(2)
        b = seq[t - 1, :2 * hh, :2 * hw, :3].astype(np.float32).mean(2)
        la = a.reshape(hh, 2, hw, 2).mean((1, 3))
        lb = b.reshape(hh, 2, hw, 2).mean((1, 3))
        by, bx = hh // blk, hw // blk
        best = np.full((by, bx), np.inf, np.float32)
        bdy = np.zeros((by, bx), np.int32)
        bdx = np.zeros((by, bx), np.int32)
        sad0 = None
        for dy in range(-search, search + 1):
            for dx in range(-search, search + 1):
                sh = np.roll(np.roll(lb, dy, 0), dx, 1)
                s = np.abs(la - sh).reshape(by, blk, bx, blk).sum((1, 3))
                if dy == 0 and dx == 0:
                    sad0 = s
                better = s < best
                best = np.where(better, s, best)
                bdy = np.where(better, dy, bdy)
                bdx = np.where(better, dx, bdx)
        zero = sad0 <= best + blk * blk * 1.0
        bdy[zero] = 0
        bdx[zero] = 0
        # full-res +-1 refinement of the doubled half-res vector (odd
        # full-res shifts are invisible to the half-res search)
        fy0 = np.repeat(np.repeat(bdy * 2, 2 * blk, 0), 2 * blk, 1)
        fx0 = np.repeat(np.repeat(bdx * 2, 2 * blk, 0), 2 * blk, 1)
        yy, xx = np.mgrid[0:2 * hh, 0:2 * hw]
        rb = np.full((by, bx), np.inf, np.float32)
        ry = np.zeros((by, bx), np.int32)
        rx = np.zeros((by, bx), np.int32)
        for ey in (-1, 0, 1):
            for ex in (-1, 0, 1):
                gy = np.clip(yy - fy0 - ey, 0, 2 * hh - 1)
                gx = np.clip(xx - fx0 - ex, 0, 2 * hw - 1)
                s = np.abs(a - b[gy, gx]).reshape(
                    by, 2 * blk, bx, 2 * blk).sum((1, 3))
                if ey == 0 and ex == 0:
                    s = s - 1e-3      # tie -> keep the unrefined vector
                better = s < rb
                rb = np.where(better, s, rb)
                ry = np.where(better, ey, ry)
                rx = np.where(better, ex, rx)
        fy = fy0 + np.repeat(np.repeat(ry, 2 * blk, 0), 2 * blk, 1)
        fx = fx0 + np.repeat(np.repeat(rx, 2 * blk, 0), 2 * blk, 1)
        sy, sx = yy - fy, xx - fx
        inb = (sy >= m) & (sy < 2 * hh - m) & (sx >= m) & (sx < 2 * hw - m)
        sy, sx = np.clip(sy, 0, 2 * hh - 1), np.clip(sx, 0, 2 * hw - 1)
        i_t = seq[t, :2 * hh, :2 * hw, :3].astype(np.int16)
        i_p = seq[t - 1, sy, sx, :3].astype(np.int16)
        o_t = outs[t, :2 * hh, :2 * hw, :3].astype(np.int16)
        o_p = outs[t - 1, sy, sx, :3].astype(np.int16)
        valid = inb & (np.abs(i_t - i_p).max(2) <= 2)
        if valid.mean() < 0.05:
            continue
        res.append(((np.abs(o_t - o_p).max(2) > 4) & valid).sum()
                   / valid.sum())
    return res


def ghost_fixture(kind, w, h):
    """Synthetic loading-screen fixture: bright text on black that slides
    left 1px/frame then stops ("slide"), or fades in then out ("fade").
    A small spinner far from the text keeps frames non-identical (the
    live layer reuses byte-identical frames, so a real loading screen
    only reaches the daemon because something animates). Returns
    (seq N,H,W,4 uint8, band slices around the text row)."""
    from PIL import Image, ImageDraw, ImageFont
    try:
        font = ImageFont.truetype("arialbd.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    ty = h - 60

    def fr(tx, alpha, spin):
        im = Image.new("RGB", (w, h), (0, 0, 0))
        d = ImageDraw.Draw(im)
        v = int(230 * alpha)
        d.text((tx, ty), "LOADING", fill=(v, v, v), font=font)
        a = spin % 8
        d.rectangle((w - 60 + a * 4, 40, w - 56 + a * 4, 44),
                    fill=(200, 200, 200))
        rgb = np.asarray(im)
        out = np.empty((h, w, 4), np.uint8)
        out[..., 0], out[..., 1], out[..., 2] = rgb[..., 2], rgb[..., 1], rgb[..., 0]
        out[..., 3] = 255
        return out

    cx = w // 2 - 40
    if kind == "slide":
        s = [fr(cx + 140 - i, 1.0, i) for i in range(120)] + \
            [fr(cx + 20, 1.0, i) for i in range(60)]
    else:
        s = [fr(cx, min(1.0, i / 30.0), i) for i in range(60)] + \
            [fr(cx, max(0.0, 1.0 - i / 30.0), i) for i in range(40)] + \
            [fr(cx, 0.0, i) for i in range(60)]
    return np.stack(s), (slice(ty - 20, ty + 40), slice(0, w - 70))


def analyze_ghost(seq, outs, band, kind):
    """Persistent residue on pixels whose input is BLACK: the hold-latch
    ghost (owner report 2026-10-10: the loading word smeared into faint
    copies; moved/faded content left <32/255 residue pinned forever).
    Scored on the last 20 frames, where the input has been settled for
    >=20 frames - any residue there is held, not transitional. Baseline
    (net's own halo on static text): ~0.1% of band px >6, max ~11/255."""
    i3 = seq[-20:, :, :, :3].astype(np.int16)[:, band[0], band[1]]
    o3 = outs[-20:, :, :, :3].astype(np.int16)[:, band[0], band[1]]
    blk = i3.max(3) <= 2
    om = o3.max(3)
    share = ((om > 6) & blk).sum() / max(blk.sum(), 1)
    mx = int(om[blk].max()) if blk.any() else 0
    notes = ["ghost[%s]: residue>6 on black-input px %.2f%%, max %d/255"
             % (kind, share * 100, mx)]
    if share > 0.01 or mx > 16:
        return "FAIL", notes + ["FAIL ghost[%s]: held residue where input "
                                "is black (phantom trail)" % kind]
    if share > 0.004:
        return "WARN", notes + ["WARN ghost[%s] residue %.2f%%"
                                % (kind, share * 100)]
    return "PASS", notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sequence", help="raw/manifest path, or "
                    "'fixture:ghost' for the synthetic loading-text gate")
    ap.add_argument("--w", type=int, default=800)
    ap.add_argument("--h", type=int, default=600)
    ap.add_argument("--port", type=int, default=48990)
    ap.add_argument("--save-out")
    args = ap.parse_args()

    if args.sequence == "fixture:ghost":
        d = Daemon(args.port, args.w, args.h)
        rank = {"PASS": 0, "WARN": 1, "FAIL": 2}
        worst = "PASS"
        for kind in ("slide", "fade"):
            seq, band = ghost_fixture(kind, args.w, args.h)
            outs = np.empty_like(seq)
            for i, fr in enumerate(seq):
                outs[i] = d.frame(fr)
            v, notes = analyze_ghost(seq, outs, band, kind)
            v2, notes2 = analyze(seq, outs, 25.0)
            for n in notes + notes2:
                print("  " + n)
            for vv in (v, v2):
                if rank[vv] > rank[worst]:
                    worst = vv
        print("[replay_gate] verdict: %s" % worst)
        return 0 if worst == "PASS" else 1

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
