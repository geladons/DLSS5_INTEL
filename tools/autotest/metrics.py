# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# metrics - single-pass per-frame metric accumulator.
#
# One MetricAccumulator instance consumes the decoded frame stream once and
# produces every scalar timeline + accumulated spatial map the detectors
# need. Full-res luma is kept for diff/patchwork fidelity; per-pixel
# temporal work runs at an analysis resolution (frame[::s,::s], s chosen so
# the width lands near --maxw) so a 200s 120fps capture stays tractable.
#
# Reused techniques from tools/gamerec/artifact_scan.py:
#   * BT601 luma from bgr24, 64-bin histogram distance, dup detection
#   * FFT phase-correlation dominant shift on 1/16-res planes
#   * rolling median/MAD z-scores (done later in detectors.py)
# ============================================================================
import zlib
from collections import deque

import numpy as np

LUMA = (0.114, 0.587, 0.299)          # b,g,r weights for bgr24 input


def luma_of(rgb):
    return (rgb[..., 0].astype(np.float32) * LUMA[0]
            + rgb[..., 1].astype(np.float32) * LUMA[1]
            + rgb[..., 2].astype(np.float32) * LUMA[2])


def gradmag(g):
    gx = np.abs(np.diff(g, axis=1, prepend=g[:, :1]))
    gy = np.abs(np.diff(g, axis=0, prepend=g[:1]))
    return gx + gy


def boxsum9(m):
    """9x9 box sum of a float/bool map via shifts (radius 4 each way is
    approximated by the 3x3 of +-1 shifts twice -> use r=4: cheap enough)."""
    f = m.astype(np.float32)
    acc = f
    for dy in (-2, -1, 0, 1, 2):
        for dx in (-2, -1, 0, 1, 2):
            if dy or dx:
                acc = acc + np.roll(f, (dy, dx), (0, 1))
    return acc


def dilate(m, it=1):
    """4-neighbour binary dilation, it iterations."""
    out = m
    for _ in range(it):
        out = (out | np.roll(out, 1, 0) | np.roll(out, -1, 0)
               | np.roll(out, 1, 1) | np.roll(out, -1, 1))
    return out


def dom_shift(prev_small, cur_small, max_r=8):
    """Dominant integer translation via FFT phase correlation
    (verbatim approach from artifact_scan.dom_shift)."""
    a = prev_small.astype(np.float32) - float(prev_small.mean())
    b = cur_small.astype(np.float32) - float(cur_small.mean())
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


class MetricAccumulator:
    """Feeds on bgr24 frames; collects timelines + spatial evidence maps."""

    def __init__(self, w, h, fps, maxw=640, dup_eps=0.15, flick_eps=2.5):
        self.w, self.h, self.fps = w, h, fps
        self.s = max(1, int(round(w / float(maxw))))
        self.dup_eps = dup_eps
        self.flick_eps = flick_eps
        self.block = 8 * self.s               # patchwork block in full px

        self.prev_full = None
        self.prev_lumas = deque(maxlen=26)    # analysis-res float32
        self.prev_half = deque(maxlen=3)      # half-analysis-res float32
        self.prev_rgbs = deque(maxlen=3)      # analysis-res int16
        self.prev_hist = None
        self.dup_run = 0
        self.upd_gaps = deque(maxlen=30)    # frames between unique frames
        self.last_upd = None                # idx of previous unique frame

        # long-term spatial accumulators (analysis res)
        self.welf_n = 0
        self.welf_mean = None
        self.welf_m2 = None
        self.sum_abs = None                   # sum |d luma| per px
        self.flick_map = None                 # weighted alternation energy
        self.flick_cnt = None                 # alternation count per px
        self.flick_run = None                 # current alternation run len
        self.patch_map = None                 # accumulated patchwork resid
        self.static_prev_mask = None

        # garland event segmentation state (hysteresis on flicker_frac)
        self.evt_open = False
        self.evt = None
        self.evt_gap = 0
        self.flicker_events = []

        self.rows = []                        # per-frame metric dicts
        self.region_luma = []                 # 9-region luma grid series

    # ------------------------------------------------------------------
    def feed(self, idx, rgb):
        s = self.s
        g_full = luma_of(rgb)
        g = g_full[::s, ::s]
        rgb_a16 = rgb[::s, ::s].astype(np.int16)   # cached int16 for flicker
        row = {"frame": idx}

        # --- scalar metrics ---
        row["luma_mean"] = float(g_full.mean())
        row["luma_std"] = float(g_full.std())
        row["cb_mean"] = float(rgb_a16[..., 0].mean()) - row["luma_mean"]
        row["cr_mean"] = float(rgb_a16[..., 2].mean()) - row["luma_mean"]
        row["hash"] = zlib.crc32(rgb.tobytes()) & 0xFFFFFFFF
        # bincount on quantised luma is ~5x faster than np.histogram
        hist = np.bincount(np.clip(g, 0, 255).astype(np.uint8).ravel(),
                           minlength=256).astype(np.float64)
        hist = hist[:256].reshape(32, 8).sum(1)          # 32 bins
        row["edge_energy"] = float(gradmag(g).mean())
        regs = []
        for yy in range(3):
            for xx in range(3):
                sub = g[yy * g.shape[0] // 3:(yy + 1) * g.shape[0] // 3,
                        xx * g.shape[1] // 3:(xx + 1) * g.shape[1] // 3]
                regs.append(float(sub.mean()))
        self.region_luma.append(regs)

        if self.prev_full is None:
            self._init_state(g, g_full, hist)
            self.prev_hash = row["hash"]
            self.last_upd = idx             # first frame anchors gap 0
            self._fill_first(row)
            self.rows.append(row)
            return row

        # --- inter-frame deltas ---
        d_full = np.abs(g_full - self.prev_full)
        row["diff_mean"] = float(d_full.mean())
        row["dup"] = int(row["diff_mean"] < self.dup_eps
                         or row["hash"] == self.prev_hash)
        self.dup_run = self.dup_run + 1 if row["dup"] else 0
        row["dup_run"] = self.dup_run
        row["hist_dist"] = float(
            1.0 - np.minimum(hist, self.prev_hist).sum()
            / max(1.0, hist.sum()))

        if row["dup"]:
            # near-identical repeat: no delta, no echo possible. The
            # unique-frame history deques are NOT appended - flicker,
            # ghost and static all operate on the unique stream so an
            # 80%-dup 120fps capture doesn't degenerate to zero deltas.
            row["patch_idx"] = 0.0
            row["ghost_echo"] = 0.0
            row["static_var"] = 0.0
            row.update({"flicker_frac": 0.0, "flicker_mag": 0.0,
                        "flicker_color": 0.0, "flicker_strong": 0.0})
            row.update({"lag_dy": 0, "lag_dx": 0, "lag_pk": 0.0,
                        "lag_gain": 0.0, "lag_resid": 0.0,
                        "lag2_resid": 0.0, "lag2_pk": 0.0, "cad_cv": 0.0})
            self._segment_event(idx, 0.0, self._zero_mag, None)
        else:
            row["patch_idx"] = self._patchwork(d_full)
            # a globally-frozen scene produces only legit stale pixels;
            # smear detection needs some live motion in the frame
            row["ghost_echo"] = (self._ghost(g)
                                 if row["diff_mean"] > 1.0 else 0.0)
            row["static_var"] = self._static_update(g)
            row.update(self._flicker(idx, rgb_a16))
            # must run before the prev_half append: [-1] is the previous
            # unique frame, [-2] the one two uniques back
            row.update(self._lag_trail(idx, g))
            self.prev_lumas.append(g.copy())
            self.prev_half.append(g[::2, ::2].copy())
            self.prev_rgbs.append(rgb_a16)

        self.prev_full = g_full
        self.prev_hash = row["hash"]
        self.prev_hist = hist
        self.rows.append(row)
        return row

    # ------------------------------------------------------------------
    def _init_state(self, g, g_full, hist):
        shape = g.shape
        self.welf_mean = np.zeros(shape, np.float32)
        self.welf_m2 = np.zeros(shape, np.float32)
        self.sum_abs = np.zeros(shape, np.float32)
        self.flick_map = np.zeros(shape, np.float64)
        self.flick_cnt = np.zeros(shape, np.int32)
        self.flick_run = np.zeros(shape, np.int32)
        # per-channel single-plane buffers: (h,w,3) ops are ~13x slower
        # than three sequential (h,w) passes on this numpy build
        self._fb = {k: np.zeros(shape, np.int16)
                    for k in ("d1", "d2", "a1", "a2", "mn", "mag")}
        self._bb = {k: np.zeros(shape, bool)
                    for k in ("up", "dn", "flip", "alt")}
        self._zero_mag = np.zeros(shape, np.float32)
        self._static_thr_t = 0.0
        self.prev_full = g_full
        self.prev_hist = hist
        self.prev_lumas.append(g.copy())
        self.prev_half.append(g[::2, ::2].copy())

    def _fill_first(self, row):
        row.update({"diff_mean": 0.0, "dup": 0, "dup_run": 0,
                    "hist_dist": 0.0, "patch_idx": 0.0, "ghost_echo": 0.0,
                    "static_var": 0.0, "flicker_frac": 0.0,
                    "flicker_mag": 0.0, "flicker_color": 0.0,
                    "flicker_strong": 0.0, "lag_dy": 0, "lag_dx": 0,
                    "lag_pk": 0.0, "lag_gain": 0.0, "lag_resid": 0.0,
                    "lag2_resid": 0.0, "lag2_pk": 0.0, "cad_cv": 0.0})

    # ------------------------------------------------------------------
    def _patchwork(self, d_full):
        """Hold/live interleave density between neighbouring 8x8 blocks of
        |frame - prev|. Raw-vs-processed block mixing puts latched blocks
        (block-mean delta ~0) directly next to live ones (delta >4) ->
        the fraction of held blocks with a live neighbour jumps to ~0.5+.
        Natural motion clusters held blocks together (static background
        with a few movers) so the fraction stays low. Accumulates a
        per-block std-contrast evidence map for the heatmap."""
        b = self.block
        h = d_full.shape[0] // b * b
        w = d_full.shape[1] // b * b
        if h < 2 * b or w < 2 * b:
            return 0.0
        blk = d_full[:h, :w].reshape(h // b, b, w // b, b)
        bm = blk.mean(axis=(1, 3))
        bs = np.sqrt(np.maximum(
            (blk * blk).mean(axis=(1, 3)) - bm * bm, 0.0))
        # signature: of all adjacencies that touch a HELD block
        # (block-mean delta ~0), what fraction touch a LIVE one. Natural
        # motion clusters held blocks together (static bg + a few movers
        # -> tiny perimeter fraction); raw/enhanced block mixing
        # interleaves them densely -> fraction jumps to ~0.5+.
        held = bm < 1.0
        live = bm > 4.0
        hp = np.pad(held, 1)
        lp = np.pad(live, 1)
        nb_held = (hp[:-2, 1:-1] | hp[2:, 1:-1] |
                   hp[1:-1, :-2] | hp[1:-1, 2:])[1:-1, 1:-1]
        nb_live = (lp[:-2, 1:-1] | lp[2:, 1:-1] |
                   lp[1:-1, :-2] | lp[1:-1, 2:])[1:-1, 1:-1]
        n_hold = int((held[1:-1, 1:-1] & nb_live).sum())
        n_any = int((held[1:-1, 1:-1] & (nb_held | nb_live)).sum())
        frac = float(n_hold) / float(n_any) if n_any else 0.0
        nb = frac if (n_hold >= 8 and n_any >= 30) else 0.0
        self._pw_pairs = (n_hold, n_any)
        # residual vs 3x3 box mean -> accumulated evidence map
        pad = np.pad(bs, 1, mode="edge")
        box = sum(pad[dy:dy + bs.shape[0], dx:dx + bs.shape[1]]
                  for dy in range(3) for dx in range(3)) / 9.0
        res = np.maximum(bs - box, 0.0)
        if self.patch_map is None or self.patch_map.shape != res.shape:
            self.patch_map = np.zeros(res.shape, np.float64)
        self.patch_map += res
        return nb

    # ------------------------------------------------------------------
    def _ghost(self, g):
        """Two signatures, summed:

        long-hold smear - pixel identical for >=8 frames now but different
        in frames 9-24 back, clustered in a 32x32 block that contains
        edges = a latched/dragged contour. A moving uniform object fails
        the >=8f hold; a forever-static pole fails the recent-change
        gate; whole-scene freezes fail the caller's diff gate.

        lagged echo - pixel matches an OLDER frame (lag 2..3) better than
        the previous one on a faded edge with a strong survivor contour
        nearby (temporal reuse artefacts like A/B re-emission).
        """
        d1 = np.abs(g - self.prev_lumas[-1])
        ge = gradmag(g)
        edge_lo = 14.0
        stale = d1 < 2.0
        if len(self.prev_lumas) >= 3:
            stale = stale & (np.abs(g - self.prev_lumas[-3]) < 2.0)
        # "long-hold smear": >=8 frames identical now, but this pixel
        # looked different somewhere in frames 9-24 back. The >=8-frame
        # hold separates a latched/dragged smear from a uniformly-coloured
        # MOVING object (a moving box's interior px match only while the
        # object covers them, <8f at any sane speed; |cur-prev8| differs
        # -> excluded). A still pole fails the changed-in-9..24 gate.
        # Whole-scene freezes are excluded by the caller's diff gate.
        m = min(8, len(self.prev_lumas))
        hold = stale & (np.abs(g - self.prev_lumas[-m]) < 2.0)
        moved_hist = np.zeros(g.shape, bool)
        for k in range(m + 1, min(25, len(self.prev_lumas) + 1)):
            moved_hist |= np.abs(g - self.prev_lumas[-k]) > 8
        gm = hold & moved_hist
        gfrac = float(gm.mean())
        damp = 1.0 if gfrac < 0.15 else max(0.05, 1.0 - (gfrac - 0.15))
        edges = ge > edge_lo
        n_edge = max(1, int(edges.sum()))
        bs = 32
        bh = gm.shape[0] // bs * bs
        bw = gm.shape[1] // bs * bs
        if bh >= bs and bw >= bs:
            gb = gm[:bh, :bw].reshape(bh // bs, bs, bw // bs, bs)
            occ = gb.mean(axis=(1, 3))
            eb = edges[:bh, :bw].reshape(bh // bs, bs, bw // bs, bs)
            has_edge = eb.sum(axis=(1, 3)) >= 8
            stale_score = float((occ * has_edge).max()) * damp
        else:
            stale_score = float((gm & edges).sum()) / n_edge * damp

        gh = g[::2, ::2]
        geh = gradmag(gh)
        edge_hi, sim = 40.0, 25.0
        weak_now = (geh > edge_lo) & (geh < edge_hi)
        n_edge_h = max(1, int((geh > edge_lo).sum()))
        strong_near = dilate(geh > edge_hi, 6)
        d_prev1 = np.abs(gh - self.prev_half[-1])
        echo = 0.0
        for lag in range(2, min(4, len(self.prev_half) + 1)):
            old = self.prev_half[-lag]
            d_old = np.abs(gh - old)
            gp2 = gradmag(old)
            gm = (weak_now & dilate(gp2 > edge_hi, 2) & strong_near
                  & (d_old < sim) & (d_old < d_prev1))
            if gm.any():
                echo = max(echo, float(gm.sum()) / n_edge_h)
        return stale_score + echo

    # ------------------------------------------------------------------
    def _static_update(self, g):
        """Welford variance + |d| accumulation; returns temporal std on the
        currently-static mask (areas that barely move but still jitter).
        The mask threshold (median of the running mean|d| map) is only
        refreshed every 16 frames - it drifts slowly."""
        d = np.abs(g - self.prev_lumas[-1])
        self.welf_n += 1
        delta = g - self.welf_mean
        self.welf_mean += delta / self.welf_n
        self.welf_m2 += delta * (g - self.welf_mean)
        self.sum_abs += d
        if self.welf_n % 16 == 1 or self._static_thr_t == 0.0:
            dev = np.abs(g - self.welf_mean)
            self._static_thr_t = max(
                9.0, float(np.median(dev)) + 6.0)
        # static = pixel near its running temporal MEAN. A jittery-but-
        # anchored pixel (loading-screen shimmer) stays in the mask even
        # though its per-frame |d1| is high - that is exactly the class
        # this metric exists to catch.
        mask = np.abs(g - self.welf_mean) < self._static_thr_t
        self.static_mask = mask
        if mask.any() and self.welf_n > 4:
            return float(d[mask].std())
        return 0.0

    # ------------------------------------------------------------------
    def _lag_trail(self, idx, g):
        """Dominant-shift coherence of the unique stream, on the
        half-analysis-res luma deque (the artifact_scan dom_shift
        pattern, ~1/8-res at 1440p):

        lag_dy/dx/pk - dominant integer translation vs the previous
        unique frame + its phase-correlation peak. pk~0 means the delta
        is not one coherent motion (re-derivation, noise, scene cut).

        lag_gain - fraction of the inter-frame residual explained by
        that single translation: r0 = mean|cur-prev|, r1 = the same
        after rolling prev by (dy,dx); gain = (r0-r1)/r0. Coherent
        motion -> >0.5; a pipeline that re-derives the frame instead of
        moving it -> ~0 even though content changed.

        lag_resid/lag2_resid/lag2_pk - lag_resid is r1 above;
        lag2_resid repeats the match against the unique frame TWO back
        (roll by its own dom_shift). A stale-application stage - delta
        of frame k composited at frame k-2's coordinates - makes the
        output track the older position: lag2_resid < lag_resid while
        lag2_pk stays high. lag_resid is kept in the row (not the CSV)
        so the detector can compare the two residuals.

        cad_cv - coefficient of variation of the last <=30 gaps between
        unique frames (in container frames). CFR delivery -> ~0;
        irregular update cadence on moving content (judder) -> high.
        """
        out = {"lag_dy": 0, "lag_dx": 0, "lag_pk": 0.0, "lag_gain": 0.0,
               "lag_resid": 0.0, "lag2_resid": 0.0, "lag2_pk": 0.0,
               "cad_cv": 0.0}
        q = g[::2, ::2]
        if self.prev_half:
            p1 = self.prev_half[-1]
            dy, dx, pk = dom_shift(p1, q)
            out["lag_dy"], out["lag_dx"], out["lag_pk"] = dy, dx, pk
            r0 = float(np.abs(q - p1).mean())
            r1 = float(np.abs(q - np.roll(p1, (dy, dx), (0, 1))).mean())
            out["lag_resid"] = r1
            out["lag_gain"] = (r0 - r1) / max(r0, 1e-6)
            if len(self.prev_half) >= 2:
                p2 = self.prev_half[-2]
                dy2, dx2, pk2 = dom_shift(p2, q)
                out["lag2_pk"] = pk2
                out["lag2_resid"] = float(
                    np.abs(q - np.roll(p2, (dy2, dx2), (0, 1))).mean())
        if self.last_upd is not None:
            self.upd_gaps.append(idx - self.last_upd)
        self.last_upd = idx
        if len(self.upd_gaps) >= 3:
            gaps = np.asarray(self.upd_gaps, np.float64)
            out["cad_cv"] = float(gaps.std() / max(gaps.mean(), 1e-6))
        return out

    # ------------------------------------------------------------------
    def _flicker(self, idx, cur):
        """Per-pixel sign-alternating deltas across 3 consecutive frames,
        per colour channel. Returns per-frame fraction/magnitude plus keeps
        the event segmentation + spatial evidence maps up to date.
        `cur` is the analysis-res int16 RGB plane (cached in prev_rgbs)."""
        out = {"flicker_frac": 0.0, "flicker_mag": 0.0,
               "flicker_color": 0.0, "flicker_strong": 0.0}
        if len(self.prev_rgbs) < 2:
            return out
        p1, p2 = self.prev_rgbs[-1], self.prev_rgbs[-2]
        eps = self.flick_eps
        # per-channel: flip = sign(d1) != sign(d2) AND min(|d1|,|d2|) >= eps
        alt_any = self._bb["alt"]
        alt_any[:] = False
        mag_i = self._fb["mag"]
        mag_i[:] = 0
        ups, dns = [], []
        for c in range(3):
            d1 = np.subtract(cur[..., c], p1[..., c], out=self._fb["d1"])
            d2 = np.subtract(p1[..., c], p2[..., c], out=self._fb["d2"])
            a1 = np.abs(d1, out=self._fb["a1"])
            a2 = np.abs(d2, out=self._fb["a2"])
            mn = np.minimum(a1, a2, out=self._fb["mn"])
            np.less(d1, 0, out=self._bb["up"])
            np.less(d2, 0, out=self._bb["dn"])
            fl = np.bitwise_xor(self._bb["up"], self._bb["dn"],
                                out=self._bb["flip"])
            fl &= mn >= eps
            alt_any |= fl
            np.multiply(mn, fl, out=self._fb["a1"])
            np.maximum(mag_i, self._fb["a1"], out=mag_i)
            np.greater(d1, eps, out=self._bb["up"])
            np.less(d1, -eps, out=self._bb["dn"])
            ups.append(self._bb["up"].copy())
            dns.append(self._bb["dn"].copy())
        frac = float(alt_any.mean())
        out["flicker_frac"] = frac
        if alt_any.any():
            out["flicker_mag"] = float(mag_i[alt_any].mean())
            # "colourful" = alternation direction differs across channels
            u0, u1, u2 = ups
            n0, n1, n2 = dns
            diff_sign = ((u0 & (n1 | n2)) | (n0 & (u1 | u2))
                         | (u1 & n2) | (n1 & u2)) & alt_any
            out["flicker_color"] = float(diff_sign.mean())
        self.flick_run = np.where(alt_any, self.flick_run + 1, 0)
        out["flicker_strong"] = float((self.flick_run >= 2).mean())
        self.flick_map += mag_i            # already 0 where no flip
        self.flick_cnt += alt_any

        self._segment_event(idx, frac, mag_i, alt_any)
        return out

    def _segment_event(self, idx, frac, mag, alt_any):
        enter, exit_, max_gap = 0.003, 0.0015, 8
        if not self.evt_open and frac >= enter:
            self.evt_open = True
            self.evt = {"start": idx, "peak": frac, "peak_frame": idx,
                        "frames": 0, "map": np.zeros(
                            self.flick_map.shape, np.float64),
                        "peak_mag": 0.0}
            self.evt_gap = 0
        if self.evt_open:
            e = self.evt
            e["frames"] += 1
            if alt_any is not None:
                e["map"] += mag * alt_any
            if frac > e["peak"]:
                e["peak"], e["peak_frame"] = frac, idx
            e["peak_mag"] = max(e["peak_mag"], float(mag.max()))
            if frac < exit_:
                self.evt_gap += 1
            else:
                self.evt_gap = 0
            if self.evt_gap > max_gap:
                self._close_event(idx)

    def _close_event(self, idx):
        e = self.evt
        e["end"] = idx - self.evt_gap
        if e["peak"] >= 0.005 and e["frames"] >= 2:
            self.flicker_events.append(e)
        self.evt_open = False
        self.evt = None

    # ------------------------------------------------------------------
    def finalize(self):
        if self.evt_open:
            self._close_event(len(self.rows))
        n = len(self.rows)
        keys = [k for k in self.rows[0] if k != "frame"]
        series = {k: np.array([r.get(k, 0.0) for r in self.rows],
                              dtype=np.float64) for k in keys}
        series["hash"] = np.array([r["hash"] for r in self.rows],
                                  dtype=np.uint32).astype(np.float64)
        uniq = np.array([i for i in range(n)
                         if i == 0 or series["dup"][i] == 0])
        static_std = (np.sqrt(self.welf_m2 / max(1, self.welf_n - 1))
                      if self.welf_m2 is not None else None)
        return {
            "n": n, "fps": self.fps, "s": self.s, "series": series,
            "region_luma": np.array(self.region_luma),
            "uniq_idx": uniq,
            "maps": {"flicker": self.flick_map, "flick_cnt": self.flick_cnt,
                     "patch": self.patch_map,
                     "static_std": static_std,
                     "static_mask": getattr(self, "static_mask", None),
                     "mean_abs": (self.sum_abs / max(1, self.welf_n)
                                  if self.sum_abs is not None else None)},
            "flicker_events": self.flicker_events,
        }
