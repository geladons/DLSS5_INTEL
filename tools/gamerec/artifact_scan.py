#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# artifact_scan - per-frame artifact detector for gamerec captures.
#
# Decodes an mp4 (any fps; designed for 120 fps ddarec captures) and scores
# every frame across the artifact classes the field reports use:
#
#   luma_pump     - brightness jumps/breathing: luma z-score vs rolling median
#   chroma_drift  - colour wander: R/B channel-mean z-score ("гуляют цвета")
#   hf_zigzag     - generated-vs-anchor character alternation: high-frequency
#                   (Laplacian) energy alternates at the emission cadence,
#                   detected on the UNIQUE-frame stream (lag-2 autocorr +
#                   per-frame outlier flag)
#   flash         - short frame-to-frame pixel-diff spike that is NOT a scene
#                   cut (cuts also shift the luma histogram heavily)
#   judder        - duplicate-frame runs longer than the expected cadence
#                   (stick-slip / held-frame stalls)
#   ghost         - uncompensated trail: dominant-motion-compensated residual
#                   energy in edge regions above the sequence baseline
#
# Output:
#   <name>.artifacts.csv   - per-frame metrics + flag bitmask
#   <name>.artifacts.txt   - console-formatted report (also printed)
#   <name>_flag_XXXX.png   - up to --dump PNGs of the worst flagged frames
#
# Usage:
#   artifact_scan.py capture.mp4 [--dump 8] [--dup-eps 0.05]
#
# numpy only (no cv2). ffmpeg decode via the same find_ffmpeg() as gamerec.
# ============================================================================
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FFMPEG_CANDIDATES = [
    r"C:\Users\<owner>\Desktop\production\tools\ffmpeg.exe",
    os.path.join(ROOT, "production", "tools", "ffmpeg.exe"),
    "ffmpeg",
]

# flag bits in the CSV
F_LUMA = 1
F_CHROMA = 2
F_HF = 4
F_FLASH = 8
F_JUDDER = 16
F_GHOST = 32


def find_ffmpeg():
    for p in FFMPEG_CANDIDATES:
        if os.path.exists(p):
            return p
    return "ffmpeg"


def probe_video(ff, video):
    probe = subprocess.run(
        [ff, "-hide_banner", "-i", video],
        capture_output=True, text=True).stderr
    m = re.search(r"(\d{2,5})x(\d{2,5})", probe)
    fm = re.search(r"(\d+(?:\.\d+)?) fps", probe)
    return int(m.group(1)), int(m.group(2)), float(fm.group(1)) if fm else 60.0


def decode_frames(ff, video, w, h):
    """Yield (idx, np.uint8 [h,w,3]) bgr24 frames."""
    import numpy as np
    sz = w * h * 3
    p = subprocess.Popen(
        [ff, "-hide_banner", "-loglevel", "error", "-i", video,
         "-f", "rawvideo", "-pix_fmt", "bgr24", "-"],
        stdout=subprocess.PIPE)
    i = 0
    while True:
        buf = p.stdout.read(sz)
        if len(buf) < sz:
            break
        yield i, np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)
        i += 1
    p.wait()


def hf_energy(g):
    """Mean |4*center - neighbours| Laplacian energy on luma plane."""
    import numpy as np
    f = g.astype(np.float32)
    lap = (4.0 * f[1:-1, 1:-1]
           - f[:-2, 1:-1] - f[2:, 1:-1] - f[1:-1, :-2] - f[1:-1, 2:])
    return float(np.abs(lap).mean())


def dom_shift(prev_small, cur_small, max_r=8):
    """Dominant integer translation between two small gray frames via
    FFT phase correlation. Returns (dy, dx, peak_strength)."""
    import numpy as np
    a = np.float32(prev_small) - np.float32(prev_small).mean()
    b = np.float32(cur_small) - np.float32(cur_small).mean()
    fa = np.fft.rfft2(a)
    fb = np.fft.rfft2(b)
    r = fa * np.conj(fb)
    r /= (np.abs(r) + 1e-6)
    c = np.fft.irfft2(r, s=a.shape)
    peak = float(np.abs(c).max())
    py, px = np.unravel_index(np.argmax(np.abs(c)), c.shape)
    h, w = a.shape
    dy = int(py - h) if py > h // 2 else int(py)
    dx = int(px - w) if px > w // 2 else int(px)
    if abs(dy) > max_r or abs(dx) > max_r:
        return 0, 0, 0.0
    return dy, dx, peak


def ghost_resid(prev, cur, dy, dx, edge_lo=30.0):
    """Motion-compensated residual energy restricted to edge pixels of
    the shifted previous frame: a moving edge whose old position still
    carries the object's colour (temporal drag / generated-frame warp
    miss) lights this up. Returns mean |diff| on the edge mask."""
    import numpy as np
    sh = np.roll(prev, (dy, dx), axis=(0, 1))
    gy = np.abs(np.diff(sh, axis=0, prepend=sh[:1]))
    gx = np.abs(np.diff(sh, axis=1, prepend=sh[:, :1]))
    mask = (gy + gx) > edge_lo
    if not mask.any():
        return 0.0
    d = np.abs(cur.astype(np.int16) - sh.astype(np.int16))
    return float(d[mask].mean())


def rolling_med_z(vals, win=45):
    """Per-point z vs rolling median/MAD (edge frames padded)."""
    import numpy as np
    a = np.asarray(vals, dtype=np.float64)
    n = len(a)
    z = np.zeros(n)
    for i in range(n):
        lo = max(0, i - win)
        seg = np.concatenate([a[lo:i], a[i + 1:min(n, i + win)]]) \
            if n > 1 else a
        med = np.median(seg)
        mad = np.median(np.abs(seg - med)) * 1.4826 + 1e-3
        z[i] = (a[i] - med) / mad
    return z


def scan(video, dump, dup_eps, outdir):
    import numpy as np
    ff = find_ffmpeg()
    w, h, fps = probe_video(ff, video)
    print("[scan] %s: %dx%d @%.0ffps" % (video, w, h, fps))

    n = 0
    luma, chroma_r, chroma_b = [], [], []
    hf, diffm, ghost_e = [], [], []
    uniq_luma = []          # for histogram-shift flash disambiguation
    prev = None
    prev_small = None
    run_dup = []            # per-frame: consecutive-dup count ending here
    dup = 0

    for i, f in decode_frames(ff, video, w, h):
        g = (f[:, :, 0].astype(np.float32) * 0.114
             + f[:, :, 1].astype(np.float32) * 0.587
             + f[:, :, 2].astype(np.float32) * 0.299)
        luma.append(float(g.mean()))
        chroma_r.append(float(f[:, :, 2].mean()) - luma[-1])
        chroma_b.append(float(f[:, :, 0].mean()) - luma[-1])
        hf.append(hf_energy(g[::4, ::4]))          # quarter-res Laplacian
        small = g[::8, ::8]
        if prev is not None:
            d = np.abs(g - prev)
            diffm.append(float(d.mean()))
            if diffm[-1] < dup_eps:
                dup += 1
            else:
                dup = 0
            dy, dx, pk = dom_shift(prev_small, small)
            ghost_e.append(ghost_resid(prev, g, dy, dx) if pk > 0.2 else 0.0)
        else:
            diffm.append(0.0)
            ghost_e.append(0.0)
        run_dup.append(dup)
        uniq_luma.append(np.histogram(g, 32, (0, 255))[0])
        prev = g
        prev_small = small
        n += 1

    if n == 0:
        sys.exit("[scan] no frames decoded")

    luma = np.array(luma); hf = np.array(hf); diffm = np.array(diffm)
    cr = np.array(chroma_r); cb = np.array(chroma_b)
    ghost_e = np.array(ghost_e); run_dup = np.array(run_dup)

    # --- unique-frame stream for alternation (120fps >> game fps) ---
    uniq_idx = [i for i in range(n) if i == 0 or run_dup[i] == 0]
    hf_u = hf[uniq_idx]

    flags = np.zeros(n, dtype=np.int32)

    # luma pump / chroma drift: rolling z-score
    zl = rolling_med_z(luma)
    zcr = rolling_med_z(cr)
    zcb = rolling_med_z(cb)
    flags[zl > 4.0] |= F_LUMA
    flags[zcr > 4.0] |= F_CHROMA
    flags[zcb > 4.0] |= F_CHROMA

    # hf zigzag: a 1:2 cadence (P,G,P,G...) shows as NEGATIVE lag-1
    # autocorrelation on the DETRENDED hf series of the unique stream.
    # Detrend via rolling median - a slow camera pan otherwise dominates
    # the raw autocorr and drowns the alternation.
    zigzag = 0.0
    if len(hf_u) > 8:
        win = 31
        kern = np.ones(win) / win
        trend = np.convolve(hf_u, kern, mode="same")
        resid = hf_u - trend
        resid[:win // 2] = resid[-win // 2:] = 0.0   # edge junk
        a1 = float((resid[:-1] * resid[1:]).mean())
        a0 = float((resid * resid).mean()) + 1e-9
        zigzag = a1 / a0       # <0 = alternation; ~0 uncorrelated
        zhf = rolling_med_z(hf_u)
        for k, ui in enumerate(uniq_idx):
            if abs(zhf[k]) > 4.0:
                flags[ui] |= F_HF

    # flash: ISOLATED diff spike - above local median AND falls back next
    # frame. A motion onset legitimately raises diff and stays raised;
    # a single-frame glitch (torn output, stale splice) spikes alone.
    # Score on the UNIQUE stream - dup frames carry diff=0 and make the
    # rolling MAD tiny, flagging every real frame during fast pans.
    diff_u = diffm[uniq_idx]
    dz = rolling_med_z(diff_u)
    for k in range(1, len(uniq_idx) - 1):
        if dz[k] > 5.0 and diff_u[k + 1] < diff_u[k] * 0.45 \
                and diff_u[k - 1] < diff_u[k] * 0.45:
            i = uniq_idx[k]
            h1 = uniq_luma[i - 1]
            h2 = uniq_luma[i]
            ov = float(np.minimum(h1, h2).sum()) / max(1.0, h1.sum())
            if ov > 0.55:      # same scene, sharp change = flash
                flags[i] |= F_FLASH

    # judder: dup-run longer than 1.8x median run among runs>=1
    runs = run_dup[run_dup > 0]
    med_run = float(np.median(runs)) if len(runs) else 0.0
    thr = max(3, med_run * 1.8)
    flags[run_dup > thr] |= F_JUDDER

    # ghost: compensated-edge residual above robust baseline, scored on
    # unique frames, and only while the frame is NOT itself a fast-motion
    # outlier - during a hard pan every edge carries honest residual.
    gu = ghost_e[uniq_idx]
    gz = rolling_med_z(gu)
    for k, ui in enumerate(uniq_idx):
        if gz[k] > 6.0 and dz[k] <= 4.0:
            flags[ui] |= F_GHOST

    # ------------------------------------------------------------- report
    base = os.path.splitext(os.path.basename(video))[0]
    os.makedirs(outdir, exist_ok=True)
    csv_path = os.path.join(outdir, base + ".artifacts.csv")
    with open(csv_path, "w") as fp:
        fp.write("frame,luma,chroma_r,chroma_b,hf,diff,dup_run,ghost,flags\n")
        for i in range(n):
            fp.write("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%d\n"
                     % (i, luma[i], cr[i], cb[i], hf[i], diffm[i],
                        run_dup[i], ghost_e[i], flags[i]))

    counts = {name: int(np.sum(flags & bit > 0))
              for name, bit in (("luma_pump", F_LUMA), ("chroma_drift", F_CHROMA),
                                ("hf_outlier", F_HF), ("flash", F_FLASH),
                                ("judder", F_JUDDER), ("ghost", F_GHOST))}
    rep = []
    rep.append("[artifact_scan] %s - %d frames %dx%d @%.0ffps"
               % (video, n, w, h, fps))
    rep.append("  unique frames: %d (dup ratio %.2f)"
               % (len(uniq_idx), 1 - len(uniq_idx) / n))
    rep.append("  hf alternation (lag-1 autocorr, detrended): %.3f"
               "  (<-0.15 suspicious, <-0.4 = real cadence flicker)"
               % zigzag)
    for k, v in counts.items():
        rep.append("  %-12s flagged: %d (%.1f%%)" % (k, v, 100.0 * v / n))
    rep.append("  luma: med=%.1f range=%.1f..%.1f p2p=%.1f"
               % (np.median(luma), luma.min(), luma.max(),
                  luma.max() - luma.min()))
    rep.append("  diff med=%.2f p95=%.2f | ghost med=%.2f p95=%.2f"
               % (np.median(diffm), np.percentile(diffm, 95),
                  np.median(ghost_e), np.percentile(ghost_e, 95)))

    flagged = np.nonzero(flags)[0]
    if len(flagged) and dump:
        # dump worst-dump frames (most flag bits, then largest |z|)
        order = sorted(flagged,
                       key=lambda i: (-bin(int(flags[i])).count("1"),
                                      -abs(zl[i])))[:dump]
        for i in order:
            fpng = os.path.join(outdir,
                                "%s_flag_%04d.png" % (base, i))
            subprocess.run([ff, "-y", "-hide_banner", "-loglevel", "error",
                            "-ss", "%.4f" % (i / fps), "-i", video,
                            "-frames:v", "1", fpng],
                           capture_output=True)
            rep.append("  dumped %s (flags=0x%x zl=%.1f)"
                       % (os.path.basename(fpng), flags[i], zl[i]))

    verdict = "CLEAN"
    if counts["flash"] or counts["judder"] or zigzag < -0.4:
        verdict = "ARTIFACTS"
    elif zigzag < -0.15 or counts["luma_pump"] or counts["ghost"] > n * 0.02:
        verdict = "SUSPECT"
    rep.append("  VERDICT: %s" % verdict)
    rep.append("  wrote %s" % csv_path)

    txt = "\n".join(rep)
    print(txt)
    with open(os.path.join(outdir, base + ".artifacts.txt"), "w") as fp:
        fp.write(txt + "\n")


def main():
    ap = argparse.ArgumentParser(prog="artifact_scan")
    ap.add_argument("video")
    ap.add_argument("--dump", type=int, default=6,
                    help="max flagged frames to export as png")
    ap.add_argument("--dup-eps", type=float, default=0.05,
                    help="mean abs diff below which a frame is a duplicate")
    ap.add_argument("--outdir", default=None)
    a = ap.parse_args()
    outdir = a.outdir or os.path.join(
        os.path.dirname(os.path.abspath(a.video)) or ".", "artifacts")
    scan(a.video, a.dump, a.dup_eps, outdir)


if __name__ == "__main__":
    main()
