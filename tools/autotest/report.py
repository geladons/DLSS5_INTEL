# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# report - writes the autotest deliverables:
#   report.md      verdict table + per-detector sections + suspects index
#   metrics.csv    per-frame metric timeline
#   events.json    machine-readable detector events
#   suspects/      full-res PNGs at the worst events (adjacent pairs for
#                  flicker so a human can blink-compare)
#   heatmap_*.png  flicker / patchwork / static-noise evidence maps
# ============================================================================
import json
import os
import re

import numpy as np

from detectors import FAIL, WARN, PASS
from pngio import heatmap_png, upscale_nn
from videoin import extract_frame

CSV_COLS = ["frame", "t", "luma_mean", "luma_std", "cb_mean", "cr_mean",
            "hash", "dup", "dup_run", "hist_dist", "diff_mean",
            "edge_energy", "flicker_frac", "flicker_mag", "flicker_color",
            "flicker_strong", "patch_idx", "ghost_echo", "static_var",
            "updates_per_s"]


def write_metrics_csv(path, ctx):
    s, fps = ctx["series"], ctx["fps"]
    upd = (s["dup"] == 0).astype(np.float64)
    win = max(1, int(round(fps)))
    rate = np.convolve(upd, np.ones(win) / win, mode="same") * fps
    with open(path, "w") as fp:
        fp.write(",".join(CSV_COLS) + "\n")
        for i in range(ctx["n"]):
            fp.write(",".join([
                str(i), "%.4f" % (i / fps),
                "%.3f" % s["luma_mean"][i], "%.3f" % s["luma_std"][i],
                "%.3f" % s["cb_mean"][i], "%.3f" % s["cr_mean"][i],
                "%d" % s["hash"][i], "%d" % s["dup"][i],
                "%d" % s["dup_run"][i], "%.4f" % s["hist_dist"][i],
                "%.3f" % s["diff_mean"][i], "%.3f" % s["edge_energy"][i],
                "%.5f" % s["flicker_frac"][i], "%.3f" % s["flicker_mag"][i],
                "%.5f" % s["flicker_color"][i],
                "%.5f" % s["flicker_strong"][i],
                "%.3f" % s["patch_idx"][i], "%.5f" % s["ghost_echo"][i],
                "%.3f" % s["static_var"][i],
                "%.1f" % rate[i]]) + "\n")


def write_heatmaps(outdir, ctx):
    maps, s = ctx["maps"], ctx["s"]
    made = []
    spec = [("flicker", "heatmap_flicker.png"),
            ("patch", "heatmap_patchwork.png"),
            ("static_std", "heatmap_static_noise.png")]
    for key, fname in spec:
        m = maps.get(key)
        if m is None or not np.any(m):
            continue
        if key == "static_std" and maps.get("static_mask") is not None:
            m = np.where(maps["static_mask"], m, 0.0)
        p = os.path.join(outdir, fname)
        heatmap_png(p, upscale_nn(m, 480))
        made.append(p)
    return made


def pick_suspects(results, budget, per_detector=6):
    """Choose the highest-scoring events across detectors; flicker events
    cost 2 slots (adjacent frame pair for blink-compare). Caps each
    detector at `per_detector` frames and skips duplicate peak frames so
    10 overlapping events don't dump the same PNG 10 times."""
    cands = []
    for r in results:
        for e in r.events:
            cands.append((e["score"], r.name, e))
    cands.sort(key=lambda c: -c[0])
    chosen, used = [], 0
    per_det, seen_frames = {}, set()
    for score, name, e in cands:
        cost = 2 if name == "garland" else 1
        f = e.get("peak_frame", (e["f0"] + e["f1"]) // 2)
        key = (name, f)
        if used + cost > budget or key in seen_frames:
            continue
        if per_det.get(name, 0) + cost > per_detector:
            continue
        chosen.append((name, e, cost))
        per_det[name] = per_det.get(name, 0) + cost
        seen_frames.add(key)
        used += cost
    return chosen


def dump_suspects(outdir, video, fps, chosen, is_dir):
    """Extract full-res PNGs. For a frames-dir input we copy the source
    PNGs instead of re-encoding."""
    import shutil
    sdir = os.path.join(outdir, "suspects")
    os.makedirs(sdir, exist_ok=True)
    dumped = []
    src_pngs = None
    if is_dir:
        src_pngs = sorted(
            (f for f in os.listdir(video)
             if f.lower().endswith(".png")),
            key=lambda f: [int(t) if t.isdigit() else t
                           for t in re.split(r"(\d+)", f)])
    for i, (name, e, cost) in enumerate(chosen):
        f = e.get("peak_frame", (e["f0"] + e["f1"]) // 2)
        tag = "%02d_%s_f%05d" % (i, name.replace("/", "_"), f)
        if is_dir:
            for k in range(cost):
                src = os.path.join(video, src_pngs[min(
                    f + k, len(src_pngs) - 1)])
                dst = os.path.join(sdir, tag + ("_b" if k else "") + ".png")
                shutil.copyfile(src, dst)
                dumped.append(dst)
        else:
            out = os.path.join(sdir, tag + ".png")
            got = extract_frame(video, f, fps, out, n_frames=cost)
            if got:
                dumped.extend(got if isinstance(got, list) else [got])
    return dumped


def write_report(path, video, ctx, results, heatmaps, suspects,
                 baseline=None):
    n, fps = ctx["n"], ctx["fps"]
    order = {FAIL: 2, WARN: 1, PASS: 0}
    overall = max((r.verdict for r in results), key=lambda v: order[v])
    L = []
    L.append("# autotest analysis report")
    L.append("")
    L.append("- source: `%s`" % video)
    L.append("- frames: %d  fps: %.3g  duration: %.1fs  analysis stride: %d"
             % (n, fps, n / fps, ctx["s"]))
    L.append("- overall verdict: **%s**" % overall)
    if baseline:
        L.append("- baseline: `%s`" % baseline["name"])
    L.append("")
    L.append("## verdict table")
    L.append("")
    L.append("| detector | verdict | score | top time ranges |")
    L.append("|---|---|---|---|")
    for r in results:
        tops = "; ".join("%.2f-%.2fs" % (e["t0"], e["t1"])
                         for e in r.top_events(3)) or "-"
        L.append("| %s | %s | %.3f | %s |"
                 % (r.name, r.verdict, r.score, tops))
    L.append("")
    for r in results:
        L.append("## %s -> %s" % (r.name, r.verdict))
        L.append("")
        for note in r.notes:
            L.append("- %s" % note)
        for e in r.top_events(3):
            ev = ", ".join("%s=%s" % (k, v) for k, v in
                           e["evidence"].items() if k != "hotspots"
                           and k != "worst_regions")
            L.append("- event f%d-f%d (%.2f-%.2fs) score %.3f: %s"
                     % (e["f0"], e["f1"], e["t0"], e["t1"], e["score"], ev))
            for hk in ("hotspots", "worst_regions"):
                for reg in e["evidence"].get(hk, [])[:3]:
                    bb = [v * ctx["s"] for v in reg["bbox"]]
                    L.append("    - hotspot bbox x0=%d y0=%d x1=%d y1=%d "
                             "peak=%.3g (full-res px, ~)"
                             % tuple(bb + [reg["peak"]]))
        L.append("")
    if baseline:
        L.append("## baseline comparison (median / p95)")
        L.append("")
        L.append("| metric | this | baseline |")
        L.append("|---|---|---|")
        b = baseline["series"]
        s = ctx["series"]
        for k in ("diff_mean", "flicker_frac", "patch_idx", "ghost_echo",
                  "static_var", "luma_mean"):
            L.append("| %s | %.4g / %.4g | %.4g / %.4g |" % (
                k, float(np.median(s[k])), float(np.percentile(s[k], 95)),
                float(np.median(b[k])), float(np.percentile(b[k], 95))))
        L.append("")
    L.append("## artifacts")
    L.append("")
    for h in heatmaps:
        L.append("- `%s`" % os.path.relpath(h, os.path.dirname(path)))
    L.append("- `metrics.csv`, `events.json`, `suspects/` (%d PNGs)"
             % len(suspects))
    L.append("")
    with open(path, "w") as fp:
        fp.write("\n".join(L))


def write_events_json(path, video, ctx, results):
    doc = {"source": video, "frames": ctx["n"], "fps": ctx["fps"],
           "detectors": {r.name: {"verdict": r.verdict, "score": r.score,
                                  "n_events": len(r.events),
                                  "notes": r.notes,
                                  "events": r.events}
                         for r in results}}
    with open(path, "w") as fp:
        json.dump(doc, fp, indent=1)
