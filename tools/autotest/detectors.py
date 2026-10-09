# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# detectors - the artifact-class detectors.
#
# Each detector consumes the shared context built by MetricAccumulator and
# returns a Result: verdict (PASS/WARN/FAIL), a scalar score, a list of
# events (frame ranges + severity + evidence), and human-readable notes.
#
#   UpdateRateDetector   - distinct-updates/sec timeline, slideshow sections
#   GarlandDetector      - per-pixel sign-alternating deltas (menu garland)
#   PumpingDetector      - global+regional luma/chroma oscillation, flashes
#   PatchworkDetector    - raw/enhanced pixel mixing in the delta map
#   GhostDetector        - lagged duplicate edges (double contour)
#   StalenessDetector    - scene cut followed by >0.5s freeze
#   StaticNoiseDetector  - motionless regions with high temporal variance
#   LagTrailDetector     - delta applied at stale coords + cadence judder
# ============================================================================
import numpy as np

PASS, WARN, FAIL = "PASS", "WARN", "FAIL"


# ---------------------------------------------------------------------- util
def rolling_med_z(vals, win=45):
    """Per-point z vs rolling median/MAD (from artifact_scan)."""
    a = np.asarray(vals, dtype=np.float64)
    n = len(a)
    z = np.zeros(n)
    for i in range(n):
        lo = max(0, i - win)
        seg = (np.concatenate([a[lo:i], a[i + 1:min(n, i + win)]])
               if n > 1 else a)
        med = np.median(seg)
        mad = np.median(np.abs(seg - med)) * 1.4826 + 1e-3
        z[i] = (a[i] - med) / mad
    return z


def _runs(mask):
    """Contiguous True-runs of a bool array -> list of (start, end_excl)."""
    out = []
    i = 0
    n = len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def _event(det, f0, f1, fps, score, **ev):
    return {"detector": det, "f0": int(f0), "f1": int(f1),
            "t0": round(f0 / fps, 3), "t1": round(f1 / fps, 3),
            "score": round(float(score), 4), "evidence": ev}


def regions_from_map(m, top=3, pct=97.0, cell=16):
    """Block-reduce a heat map to `cell` px cells, take the hot cells,
    4-connect them, return up to `top` bboxes (x0,y0,x1,y1 in map px) +
    peak value. Used for garland/patchwork/static hotspot localisation."""
    m = np.asarray(m, dtype=np.float64)
    h, w = m.shape
    bh, bw = max(1, h // cell), max(1, w // cell)
    bm = m[:bh * cell, :bw * cell].reshape(bh, cell, bw, cell).mean((1, 3))
    flat = bm[bm > 0]
    if flat.size == 0:
        return []
    thr = np.percentile(flat, pct)
    hot = bm >= max(thr, flat.mean())
    seen = np.zeros_like(hot, bool)
    comps = []
    for y in range(bh):
        for x in range(bw):
            if hot[y, x] and not seen[y, x]:
                stack, cells = [(y, x)], []
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    cells.append((cy, cx))
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = cy + dy, cx + dx
                        if (0 <= ny < bh and 0 <= nx < bw
                                and hot[ny, nx] and not seen[ny, nx]):
                            seen[ny, nx] = True
                            stack.append((ny, nx))
                ys = [c[0] for c in cells]
                xs = [c[1] for c in cells]
                peak = max(bm[c] for c in cells)
                comps.append({"bbox": [min(xs) * cell, min(ys) * cell,
                                       (max(xs) + 1) * cell,
                                       (max(ys) + 1) * cell],
                              "cells": len(cells), "peak": float(peak)})
    comps.sort(key=lambda c: -c["peak"])
    return comps[:top]


class Result:
    def __init__(self, name, verdict, score, events, notes):
        self.name, self.verdict = name, verdict
        self.score = min(1.0, max(0.0, float(score)))
        self.events, self.notes = events, notes

    def top_events(self, k=3):
        return sorted(self.events, key=lambda e: -e["score"])[:k]


# ------------------------------------------------------------------ 1 update
class UpdateRateDetector:
    name = "update-rate"

    def run(self, ctx):
        s = ctx["series"]
        fps = ctx["fps"]
        upd = (s["dup"] == 0).astype(np.float64)
        upd[0] = 0.0  # first frame is not an "update"
        win = max(1, int(round(fps)))
        rate = np.convolve(upd, np.ones(win) / win, mode="same") * fps
        med_rate = float(np.median(rate[rate > 0])) if (rate > 0).any() else 0.0
        low_thr = min(8.0, med_rate * 0.30)
        low = rate < low_thr
        events = []
        for a, b in _runs(low):
            dur = (b - a) / fps
            if dur >= 0.75 and rate[a:b].max() < low_thr:
                events.append(_event(
                    self.name, a, b, fps, (low_thr - rate[a:b].mean()) / low_thr,
                    updates_per_s=round(float(rate[a:b].mean()), 2),
                    duration_s=round(dur, 2)))
        t_low = sum((e["f1"] - e["f0"]) for e in events) / fps
        frac = t_low / max(1e-9, ctx["n"] / fps)
        verdict = PASS
        if events and (frac > 0.10 or max((e["f1"] - e["f0"])
                                          for e in events) > 3 * fps):
            verdict = FAIL
        elif events:
            verdict = WARN
        # cursor-response bound: on a nearly-static scene (menu idle) the
        # only updates ARE the cursor - the inter-unique gap distribution
        # is then the honest bound on felt input->pixel latency (field
        # report 2026-10-09: menu mouse "can't keep up" at ~15/s cadence)
        gaps = np.diff(ctx["uniq_idx"]).astype(np.float64)
        du = s["diff_mean"][ctx["uniq_idx"]]
        calm = du[:-1] < 1.0 if len(du) > 1 else np.zeros(0, bool)
        calm_gaps = gaps[calm] if len(calm) else np.array([])
        cur_note = ("cursor bound (calm scenes): inter-update gap p95 "
                    "%.0f ms, max %.0f ms"
                    % (float(np.percentile(calm_gaps, 95)) * 1000 / fps,
                       float(calm_gaps.max()) * 1000 / fps)
                    if calm_gaps.size else
                    "cursor bound: no calm segments in clip")
        notes = ["median distinct-update rate: %.1f/s  (low threshold %.1f/s)"
                 % (med_rate, low_thr),
                 "time below threshold: %.1fs (%.0f%% of clip)"
                 % (t_low, 100 * frac),
                 cur_note]
        return Result(self.name, verdict, frac, events, notes)


# ----------------------------------------------------------------- 2 garland
class GarlandDetector:
    name = "garland"

    def run(self, ctx):
        fps = ctx["fps"]
        evs = []
        for e in ctx["flicker_events"]:
            regs = regions_from_map(e["map"], top=3, cell=16)
            ev = _event(self.name, e["start"], e["end"], fps,
                        min(1.0, e["peak"] * 20 + e["peak_mag"] / 60.0),
                        peak_frac=round(e["peak"], 5),
                        peak_mag=round(e["peak_mag"], 2),
                        duration_s=round((e["end"] - e["start"]) / fps, 3),
                        hotspots=regs)
            ev["peak_frame"] = int(e["peak_frame"])
            evs.append(ev)
        frac_s = ctx["series"]["flicker_frac"]
        col_s = ctx["series"]["flicker_color"]
        n_bad = int((frac_s > 0.02).sum())
        verdict = PASS
        if n_bad > fps * 0.25 or (evs and max(
                (e["f1"] - e["f0"]) for e in evs) > fps):
            verdict = FAIL
        elif evs or frac_s.max() > 0.01:
            verdict = WARN
        notes = [
            "frames with >2%% pixels alternating: %d (%.1f%%)"
            % (n_bad, 100.0 * n_bad / max(1, ctx["n"])),
            "peak flicker fraction: %.2f%% of pixels; colourful (per-channel "
            "sign split) peak: %.2f%%"
            % (100 * float(frac_s.max()), 100 * float(col_s.max())),
            "segmented flicker events: %d" % len(evs)]
        return Result(self.name, verdict, float(frac_s.max()), evs, notes)


# ----------------------------------------------------------------- 3 pumping
class PumpingDetector:
    name = "luma-chroma-pumping"

    def _dominant_period(self, x, fps):
        """Detrended FFT: returns (freq_hz, amplitude, peakiness) of the
        strongest component in 0.3..15 Hz, or None."""
        n = len(x)
        if n < max(32, min(fps, 45)):
            return None
        k = np.ones(min(31, n // 3 * 2 + 1)) / min(31, n // 3 * 2 + 1)
        trend = np.convolve(x, k, mode="same")
        resid = x - trend
        resid[:15] = resid[-15:] = 0.0
        w = np.hanning(n)
        sp = np.abs(np.fft.rfft(resid * w))
        freqs = np.fft.rfftfreq(n, 1.0 / fps)
        band = (freqs >= 0.3) & (freqs <= min(15.0, fps / 2.1))
        if not band.any() or sp[band].size < 4:
            return None
        idx = np.argmax(sp[band])
        fband = freqs[band]
        med = np.median(sp[band]) + 1e-9
        amp = 2.0 * sp[band][idx] / (n * 0.5)
        return float(fband[idx]), float(amp), float(sp[band][idx] / med)

    def _windowed_pumps(self, x, fps, amp_thr):
        """Sliding 4s-window pump detection: a 1-2s breathing burst is
        invisible in a whole-clip FFT but dominates a short window."""
        n = len(x)
        W = min(n, int(4 * fps))
        hop = max(1, W // 4)
        hits = []
        for st in range(0, max(1, n - W + 1), hop):
            seg = x[st:st + W]
            if len(seg) < W * 0.5:
                break
            r = self._dominant_period(seg, fps)
            if r and r[1] > amp_thr and r[2] > 8.0:
                hits.append([st, st + len(seg), r])
        merged = []
        for h in hits:
            if merged and h[0] - merged[-1][1] <= hop:
                merged[-1][1] = h[1]
                if h[2][1] > merged[-1][2][1]:
                    merged[-1][2] = h[2]
            else:
                merged.append(h)
        return merged

    def run(self, ctx):
        s, fps, n = ctx["series"], ctx["fps"], ctx["n"]
        evs, notes = [], []
        uniq = ctx["uniq_idx"]

        # --- periodic pumping on global luma, cb/cr, and 3x3 regions ---
        probes = [("luma", s["luma_mean"], 0.8),
                  ("cb", s["cb_mean"], 0.6), ("cr", s["cr_mean"], 0.6)]
        for j in range(ctx["region_luma"].shape[1]):
            probes.append(("region%d" % j,
                           ctx["region_luma"][:, j], 1.2))
        for label, x, amp_thr in probes:
            for a, b, r in self._windowed_pumps(x, fps, amp_thr):
                f, amp, pk = r
                evs.append(_event(
                    self.name, a, b, fps, min(1.0, amp / 8.0),
                    channel=label, freq_hz=round(f, 2),
                    amplitude=round(amp, 2), peakiness=round(pk, 1)))
                notes.append("pump %s f%d-f%d: %.2f Hz amp=%.2f "
                             "peakiness=%.1f" % (label, a, b, f, amp, pk))

        # --- isolated one-frame flashes (raw/enhanced swap candidates) ---
        lu = s["luma_mean"][uniq]
        zl = rolling_med_z(lu)
        flashes = []
        for k in range(1, len(uniq) - 1):
            if abs(zl[k]) > 6.0 and abs(zl[k - 1]) < 3 and abs(zl[k + 1]) < 3:
                i = int(uniq[k])
                flashes.append(i)
                evs.append(_event(
                    self.name, i, i + 1, fps, min(1.0, abs(zl[k]) / 15.0),
                    kind="flash", z=round(float(zl[k]), 1),
                    luma=round(float(s["luma_mean"][i]), 2)))
        verdict = PASS
        pumps = [e for e in evs if "freq_hz" in e["evidence"]]
        if len(flashes) >= 2 or any(e["evidence"]["amplitude"] > 3.0
                                    for e in pumps):
            verdict = FAIL
        elif evs:
            verdict = WARN
        notes.insert(0, "periodic pumping channels: %d; isolated flashes: %d"
                     % (len(pumps), len(flashes)))
        return Result(self.name, verdict,
                      float(np.abs(zl).max()) / 15.0 if len(zl) else 0.0,
                      evs, notes)


# ---------------------------------------------------------------- 4 patchwork
class PatchworkDetector:
    name = "spatial-patchwork"

    def run(self, ctx):
        s, fps = ctx["series"], ctx["fps"]
        z = rolling_med_z(s["patch_idx"])
        # patch_idx = fraction of held blocks that sit next to a live one
        # (dense hold/live interleave = checkerboarded delta map). It is
        # self-normalizing, so gate on it directly; a rolling-median z
        # would absorb a persistent defect after a few frames.
        hot = (s["patch_idx"] > 0.50) & (s["diff_mean"] > 0.3)
        pk = s["patch_idx"]
        evs = []
        for a, b in _runs(hot):
            if b - a >= 2 or pk[a:b].max() > 0.75:
                sev = min(1.0, pk[a:b].mean() * 1.4)
                evs.append(_event(
                    self.name, a, b, fps, sev,
                    max_idx=round(float(pk[a:b].max()), 3),
                    mean_idx=round(float(pk[a:b].mean()), 3)))
        frac = float(hot.mean())
        verdict = PASS
        if frac > 0.005 or pk.max() > 0.8:
            verdict = FAIL if frac > 0.03 or pk.max() > 0.9 else WARN
        elif evs:
            verdict = WARN
        notes = ["patchwork idx median %.3f p95 %.3f; frames >0.50: %d"
                 % (float(np.median(pk)),
                    float(np.percentile(pk, 95)),
                    int(hot.sum()))]
        return Result(self.name, verdict,
                      min(1.0, float(pk.max())), evs, notes)


# ------------------------------------------------------------------- 5 ghost
class GhostDetector:
    name = "ghost-double-contour"

    def run(self, ctx):
        s, fps = ctx["series"], ctx["fps"]
        uniq = ctx["uniq_idx"]
        gu = s["ghost_echo"][uniq]
        z = rolling_med_z(gu, win=31)
        q25 = float(np.percentile(gu, 25)) if len(gu) else 0.0
        floor = max(0.10, q25 * 5.0)       # absolute block-occ floor
        raw = np.array([z[k] > 8.0 or (z[k] > 3.0 and gu[k] > floor)
                        for k in range(len(uniq))])
        # merge flagged unique frames into events (gap <=6 frames in the
        # raw timeline), then drop 1-frame blips
        idx_of = np.zeros(ctx["n"], bool)
        idx_of[uniq[raw]] = True
        evs = []
        # dilate by up to 6 frames so close flags merge into one event
        dil = idx_of.copy()
        for _ in range(6):
            dil[1:] |= dil[:-1]
            dil[:-1] |= dil[1:]
        for a, b in _runs(dil):
            seg = np.nonzero(idx_of[a:b])[0]
            if len(seg) < 2 and b - a < 3:
                continue
            evs.append(_event(
                self.name, a, b, fps,
                min(1.0, s["ghost_echo"][a:b].max() * 30.0),
                max_echo=round(float(s["ghost_echo"][a:b].max()), 4),
                frames=b - a))
        flagged = np.nonzero(idx_of)[0]
        frac = len(flagged) / max(1, len(uniq))
        # chronic elevation: if most unique frames carry echo while the
        # scene is actually moving, the hold pattern is systemic (e.g.
        # FG-held anchors) - worth at least a warning even though a
        # deviation z-score cannot fire on a raised baseline.
        du = s["diff_mean"][uniq]
        mov = du > 1.0
        chronic = (float((gu[mov] > floor).mean())
                   if mov.any() else 0.0)
        verdict = PASS
        if frac > 0.02 or (chronic > 0.8 and frac > 0):
            verdict = FAIL
        elif len(flagged) or chronic > 0.5:
            verdict = WARN
        notes = ["echo frames: %d/%d unique (%.1f%%); echo median %.4f "
                 "p95 %.4f; chronic(>floor, moving) %.2f"
                 % (len(flagged), len(uniq), 100 * frac,
                    float(np.median(gu)) if len(gu) else 0.0,
                    float(np.percentile(gu, 95)) if len(gu) else 0.0,
                    chronic)]
        return Result(self.name, verdict,
                      max(frac, chronic * 0.6), evs, notes)


# ---------------------------------------------------------------- 6 staleness
class StalenessDetector:
    name = "scene-change-staleness"

    def run(self, ctx):
        s, fps, n = ctx["series"], ctx["fps"], ctx["n"]
        cuts = set(np.nonzero(s["hist_dist"] > 0.35)[0])
        min_freeze = int(0.5 * fps)
        # relaxed freeze mask: after a hard cut the encoder takes ~10
        # frames to converge, so strict dup-runs start late. diff<0.75
        # covers the settle tail; a strict-dup core inside still required.
        frozen = s["diff_mean"] < 0.75
        strict_dup = s["dup"] == 1
        evs = []
        for a, b in _runs(frozen):
            length = b - a
            if length < min_freeze:
                continue
            core = sum(min(y, b) - max(x, a) for x, y in _runs(strict_dup)
                       if x < b and y > a)
            if core < 0.4 * length:
                continue
            near_cut = any((a - 20) <= c <= (a + 8) for c in cuts)
            after = (s["diff_mean"][b:b + int(fps)].mean()
                     if b < n else 0.0)
            before = s["diff_mean"][max(0, a - int(fps)):a].mean()
            moved = after > 0.5 and before > 0.5
            if near_cut and moved:
                evs.append(_event(
                    self.name, a, b, fps, min(1.0, length / (3 * fps)),
                    freeze_s=round(length / fps, 2),
                    hist_jump=round(float(s["hist_dist"]
                                          [max(0, a - 2):a + 2].max()), 3)))
        verdict = PASS
        if len(evs) > 3 or any(e["evidence"]["freeze_s"] > 3.0 for e in evs):
            verdict = FAIL
        elif evs:
            verdict = WARN
        notes = ["scene cuts (hist_dist>0.35): %d; cut->freeze events: %d"
                 % (len(cuts), len(evs))]
        return Result(self.name, verdict,
                      len(evs) / 5.0, evs, notes)


# ------------------------------------------------------------- 7 static noise
class StaticNoiseDetector:
    name = "static-region-noise"

    def run(self, ctx):
        s, fps = ctx["series"], ctx["fps"]
        z = rolling_med_z(s["static_var"])
        # absolute gate (luma units) so a chronically noisy-static clip
        # yields event windows, plus the z for bursts above baseline
        hot = (s["static_var"] > 3.0) | (z > 4.0)
        evs = []
        maps = ctx["maps"]
        regs = (regions_from_map(
                    np.where(maps["static_mask"], maps["static_std"], 0.0),
                    top=3, cell=16)
                if maps["static_std"] is not None
                and maps["static_mask"] is not None else [])
        for a, b in _runs(hot):
            if b - a >= max(2, int(0.2 * fps)):
                evs.append(_event(
                    self.name, a, b, fps, min(1.0, z[a:b].max() / 10.0),
                    max_z=round(float(z[a:b].max()), 1),
                    static_var=round(
                        float(s["static_var"][a:b].max()), 2),
                    worst_regions=regs))
        p95 = float(np.percentile(s["static_var"], 95))
        verdict = PASS
        if p95 > 3.0 or len(evs) > 3:
            verdict = FAIL
        elif evs or p95 > 1.2:
            verdict = WARN
        notes = ["static-mask temporal std p95=%.2f luma units; noisy-static "
                 "windows: %d" % (p95, len(evs))]
        return Result(self.name, verdict, p95 / 4.0, evs, notes)


# ----------------------------------------------------------------- 8 lag trail
class LagTrailDetector:
    """Two delivery-pipeline faults on the unique stream:

    stale-apply (mask lag) - the frame's delta is computed from fresh
    content but composited at the position of an OLDER anchor: output
    then matches the frame TWO uniques back (after dominant-shift
    compensation) better than the frame one back. Per-frame signature:
    lag2_pk > 0.25 (the two-back match is still a coherent translation,
    not noise) AND lag2_resid < 0.85 * lag_resid AND diff_mean > 1.0
    (content actually moved); lag_resid > 0.5 keeps codec-quantization
    noise floors from qualifying on a relative win of nothing.

    judder - irregular update delivery on moving content: inter-update
    gap CV (cad_cv) p95 > 0.8 while the unique-stream median diff_mean
    is > 0.5 (a static screen's dup stream can't be "judder").
    """
    name = "lag-trail-stale-apply"

    def run(self, ctx):
        s, fps, n = ctx["series"], ctx["fps"], ctx["n"]
        uniq = ctx["uniq_idx"]
        r1 = s["lag_resid"][uniq]
        r2 = s["lag2_resid"][uniq]
        pk2 = s["lag2_pk"][uniq]
        du = s["diff_mean"][uniq]
        moving = du > 1.0
        stale = moving & (pk2 > 0.25) & (r1 > 0.5) & (r2 < 0.85 * r1)

        # unique-frame flags -> raw timeline, dilate <=6f to merge near
        # flags into one event, drop 1-frame blips (GhostDetector conv.)
        idx_of = np.zeros(n, bool)
        idx_of[uniq[stale]] = True
        dil = idx_of.copy()
        for _ in range(6):
            dil[1:] |= dil[:-1]
            dil[:-1] |= dil[1:]
        evs = []
        for a, b in _runs(dil):
            seg = np.nonzero(idx_of[a:b])[0]
            if len(seg) < 2 and b - a < 3:
                continue
            u = uniq[(uniq >= a) & (uniq < b)]
            m1 = float(np.median(s["lag_resid"][u]))
            m2 = float(np.median(s["lag2_resid"][u]))
            margin = (m1 - m2) / max(m1, 1e-6)
            # External splice split: a parallel raw surface replays a
            # frame byte-exactly (GTA IV menu-select: flagged frame is
            # pixel-identical to the earlier occurrence, lag2_resid ~0).
            # A true stale-coordinate residual lands on CHANGED content so
            # its two-back residual stays meaningfully nonzero. Marker
            # corroborates on raw captures (h264 smears the 2px stamp).
            replay = m2 < 0.25
            marked = s["nrmark"][u] > 0 if "nrmark" in s else None
            unmarked = int((~marked).sum()) if marked is not None else 0
            ext = replay
            kind = ("external-raw-splice"
                    if (replay
                        and (marked is None or unmarked > len(u) // 2))
                    else "stale-apply")
            evs.append(_event(
                self.name, a, b, fps,
                min(1.0, margin * 2.0 + len(seg) / 30.0),
                kind=kind,
                lag2_pk=round(float(np.median(s["lag2_pk"][u])), 3),
                lag_resid=round(m1, 3), lag2_resid=round(m2, 3),
                margin_pct=round(100.0 * margin, 1), frames=b - a,
                replayed=int(replay), unmarked=unmarked))

        # judder is a clip-level stat: flag the runs where the rolling
        # gap CV is hot while content is moving
        cvu = s["cad_cv"][uniq]
        cvu = cvu[cvu > 0] if (cvu > 0).any() else cvu
        cad_p95 = float(np.percentile(cvu, 95)) if len(cvu) else 0.0
        diff_med = float(np.median(du)) if len(du) else 0.0
        judder = cad_p95 > 0.8 and diff_med > 0.5
        if judder:
            jmask = (s["cad_cv"] > 0.8) & (s["diff_mean"] > 0.5)
            for a, b in _runs(jmask):
                if b - a < max(2, int(0.1 * fps)):
                    continue
                evs.append(_event(
                    self.name, a, b, fps, min(1.0, cad_p95 / 1.6),
                    kind="judder",
                    cad_cv_max=round(float(s["cad_cv"][a:b].max()), 3),
                    cad_cv_p95=round(cad_p95, 3),
                    diff_med=round(diff_med, 3), frames=b - a))

        t_evt = sum(e["f1"] - e["f0"] for e in evs) / fps
        top = max((e["score"] for e in evs), default=0.0)
        verdict = PASS
        if evs:
            verdict = FAIL if (t_evt > 1.5 or top >= 0.75) else WARN
        elif judder or cad_p95 > 0.5:
            verdict = WARN
        n_stale = int(stale.sum())
        nr = s.get("nrmark")
        unmarked = int((nr[uniq[stale]] <= 0).sum()) if nr is not None else -1
        notes = [
            "marker coverage: %d/%d unique frames carry NRMK; %d of %d "
            "flagged frames are UNMARKED (external raw splice, bypasses "
            "every layer path)" % (
                int((nr[uniq] > 0).sum()) if nr is not None else -1,
                len(uniq), unmarked, n_stale),
            "stale-apply unique frames: %d/%d (%.1f%%); flagged event "
            "time %.2fs" % (n_stale, len(uniq),
                            100.0 * n_stale / max(1, len(uniq)), t_evt),
            "lag coherence: lag_pk median %.3f, lag_gain median %.3f "
            "(fraction of delta that is one translation)"
            % (float(np.median(s["lag_pk"][uniq])) if len(uniq) else 0.0,
               float(np.median(s["lag_gain"][uniq])) if len(uniq) else 0.0),
            "update-gap CV p95 %.3f, unique diff median %.3f -> %s"
            % (cad_p95, diff_med, "JUDDER" if judder else "regular")]
        return Result(self.name, verdict,
                      max(top, min(1.0, t_evt / 4.0)), evs, notes)


# ------------------------------------------------- 9 static-scene flicker
class StaticFlickerDetector:
    """Owner defect class 2026-10-09: on a FULLY STATIC scene (menu idle,
    no cursor motion) single frames flash a different state. The proven
    GTA IV menu-select variant is the external raw splice - an unmarked
    byte-exact replay of an older frame - but a stale-serve pop lands the
    same signature minus the replay. Both are defects: on content whose
    local baseline is calm, ANY isolated frame spike is wrong.

    Unique-stream test: rolling median of diff_mean over +-15 uniques
    marks the calm baseline (<0.5); a frame spikes if its diff exceeds
    max(baseline+1.5, 1.5) OR it carries no NRMK marker while changing
    visibly (the parallel surface never stamps). Events classify as
    'external-raw-splice' when the two-back replay signature holds
    (lag2_resid<0.25), else 'static-flicker'.
    """
    name = "static-scene-flicker"

    def run(self, ctx):
        s, fps, n = ctx["series"], ctx["fps"], ctx["n"]
        uniq = ctx["uniq_idx"]
        du = s["diff_mean"][uniq]
        if len(uniq) < 9:
            return Result(self.name, PASS, 0.0, [],
                          ["clip too short for static-flicker test"])
        # A static SCENE does not mean calm diffs: the flicker itself is
        # the diff. The real signature is a closed state space - every
        # unique frame replays its 1-back or 2-back neighbour (the menu
        # ping-pongs between two rendered states). Slow REAL motion emits
        # uniques matching neither and exits the test on its own.
        rep = np.minimum(s["lag_resid"][uniq], s["lag2_resid"][uniq])
        rep[:3] = 1.0               # no 1/2-back history -> not a replay
        win = min(31, len(rep) | 1)
        pad = np.pad(rep, win // 2, mode="edge")
        med = np.median(
            np.lib.stride_tricks.sliding_window_view(pad, win), axis=1)
        static = med < 0.25
        spikes = static & (du > 1.5)
        spikes[:3] = False          # first uniques have no lag history
        if "nrmark" in s:
            spikes |= static & (du > 0.75) & (s["nrmark"][uniq] <= 0)
        idx_of = np.zeros(n, bool)
        idx_of[uniq[spikes]] = True
        dil = idx_of.copy()
        for _ in range(4):
            dil[1:] |= dil[:-1]
            dil[:-1] |= dil[1:]
        evs = []
        n_ext = n_flick = 0
        for a, b in _runs(dil):
            seg = np.nonzero(idx_of[a:b])[0]
            if not len(seg):
                continue
            u = uniq[(uniq >= a) & (uniq < b)]
            m2 = float(np.median(s["lag2_resid"][u]))
            ext = m2 < 0.25
            kind = "external-raw-splice" if ext else "static-flicker"
            n_ext, n_flick = n_ext + ext, n_flick + (not ext)
            evs.append(_event(
                self.name, a, b, fps,
                min(1.0, float(du[(uniq >= a) & (uniq < b)].max()) / 20.0),
                kind=kind, lag2_resid=round(m2, 3),
                diff_max=round(float(du[(uniq >= a) & (uniq < b)].max()), 2),
                frames=b - a))
        t_evt = sum(e["f1"] - e["f0"] for e in evs) / fps
        verdict = PASS
        if len(evs) > 4 or t_evt > 1.0:
            verdict = FAIL
        elif evs:
            verdict = WARN
        notes = ["calm-scene spikes: %d events (%.2fs) - %d "
                 "external-raw-splice, %d static-flicker"
                 % (len(evs), t_evt, n_ext, n_flick)]
        return Result(self.name, verdict,
                      min(1.0, t_evt / 3.0), evs, notes)


# ------------------------------------------------------- 10 phantom spot
class PhantomSpotDetector:
    """Owner defect class 2026-10-09: rare (2-3 per benchmark) transient
    NOISE BLOBS - a compact region flashes anomalous content for 1-3
    unique frames while the rest of the frame is calm. Signature: the
    hottest 8x8 block delta (spot_max) concentrates >>6x the frame-mean
    delta AND the flag run is short. Two persistence gates separate the
    blob from legit transient content: the flag must spike >2x over the
    LOCAL rolling median of spot_max (a sustained mover raises its own
    baseline to ~1x), and the spot's region must go QUIET - no other hot
    blocks within +-112 px over the surrounding uniques outside the run
    itself (a real flash/explosion approaches and fades through its
    neighbourhood; verified 2026-10-09 vs the s45 explosion frames).
    """
    name = "phantom-spot"

    def run(self, ctx):
        s, fps, n = ctx["series"], ctx["fps"], ctx["n"]
        uniq = ctx["uniq_idx"]
        if "spot_max" not in s or len(uniq) < 5:
            return Result(self.name, PASS, 0.0, [],
                          ["no spot metrics in this metrics build"])
        sm = s["spot_max"][uniq]
        du = s["diff_mean"][uniq]
        conc = sm > np.maximum(du * 6.0, 12.0)
        win = min(31, len(sm) | 1)
        pad = np.pad(sm, win // 2, mode="edge")
        med = np.median(
            np.lib.stride_tricks.sliding_window_view(pad, win), axis=1)
        flag = conc & (sm > 2.0 * med)
        # keep only ISOLATED spots: <=3 consecutive flagged uniques AND
        # the region goes quiet afterwards. A REAL transient (muzzle
        # flash, explosion) persists: its hot block keeps firing in the
        # same neighbourhood for the next uniques - the phantom pops
        # once and the region reverts to calm.
        idx_of = np.zeros(n, bool)
        idx_of[uniq[flag]] = True
        sx_u, sy_u = s["spot_x"][uniq], s["spot_y"][uniq]
        upos = {int(f): k for k, f in enumerate(uniq)}

        def region_reignites(pk, pkval, run_lo, run_hi):
            """Count hot blocks near the peak's region OUTSIDE its own
            flagged run: a real transient (explosion) approaches and
            fades with many hot neighbours; a phantom run only ever
            contains itself."""
            pu = upos.get(pk)
            if pu is None:
                return False
            bx, by = sx_u[pu], sy_u[pu]
            hot = 0
            for k in range(max(0, pu - 8), min(pu + 9, len(uniq))):
                if run_lo <= uniq[k] < run_hi:
                    continue
                if (sm[k] > 0.4 * pkval
                        and abs(sx_u[k] - bx) < 112
                        and abs(sy_u[k] - by) < 112):
                    hot += 1
            return hot >= 2

        evs = []
        for a, b in _runs(idx_of):
            flagged = np.nonzero(idx_of[a:b])[0] + a
            u = uniq[(uniq >= a) & (uniq < b)]
            if len(flagged) > 3 or not len(u):
                continue
            pk = int(u[np.argmax(s["spot_max"][u])])
            if region_reignites(pk, float(s["spot_max"][pk]), a, b):
                continue
            bx, by = s["spot_x"][pk], s["spot_y"][pk]
            blk = 8 * ctx["s"]
            evs.append(_event(
                self.name, a, b, fps,
                min(1.0, float(s["spot_max"][u].max()) / 60.0),
                kind="phantom-spot",
                spot_max=round(float(s["spot_max"][u].max()), 2),
                frame_mean=round(float(du[(uniq >= a) & (uniq < b)].mean()), 2),
                bbox=[int(bx), int(by), int(bx + blk), int(by + blk)],
                peak_frame=pk))
        verdict = PASS
        if len(evs) > 6 or any(e["evidence"]["spot_max"] > 60
                               for e in evs):
            verdict = FAIL
        elif evs:
            verdict = WARN
        notes = ["isolated concentrated spikes: %d (%.1f%% of unique "
                 "frames); largest block delta %.1f luma"
                 % (len(evs), 100.0 * flag.mean() if len(flag) else 0.0,
                    float(sm.max()) if len(sm) else 0.0)]
        top = max((e["score"] for e in evs), default=0.0)
        return Result(self.name, verdict, top, evs, notes)


ALL_DETECTORS = [UpdateRateDetector, GarlandDetector, PumpingDetector,
                 PatchworkDetector, GhostDetector, StalenessDetector,
                 StaticNoiseDetector, LagTrailDetector,
                 StaticFlickerDetector, PhantomSpotDetector]
