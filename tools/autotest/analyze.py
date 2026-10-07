#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# analyze.py - automated game-quality analysis rig (analysis half).
#
#   python analyze.py <video.mp4|frames_dir> --out <reportdir>
#                     [--baseline <other.mp4>] [--fps 60] [--maxw 640]
#                     [--suspects 30] [--keep-frames <dir>]
#
# Pipeline: decode (ffmpeg rawvideo pipe) -> MetricAccumulator single pass
# (per-frame scalar timeline + accumulated spatial maps) -> seven artifact
# detectors -> report.md / metrics.csv / events.json / suspects/ / heatmaps.
#
# numpy only (cv2 optional but not required). Reuses the decode/probe/
# z-score machinery proven in tools/gamerec (see AGENTS.md).
# ============================================================================
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from metrics import MetricAccumulator            # noqa: E402
from detectors import ALL_DETECTORS              # noqa: E402
from report import (write_metrics_csv, write_heatmaps, pick_suspects,  # noqa: E402
                    dump_suspects, write_report, write_events_json)
from videoin import FrameSource                  # noqa: E402


def run_pass(source_path, args, keep_dir=None, label="main"):
    """One full decode+accumulate pass. Returns (ctx, FrameSource)."""
    src = FrameSource(source_path, fps=args.fps)
    acc = MetricAccumulator(src.w, src.h, src.fps, maxw=args.maxw,
                            dup_eps=args.dup_eps,
                            flick_eps=args.flicker_eps)
    t0 = time.time()
    n = 0
    for idx, f in src.frames(keep_dir=keep_dir):
        acc.feed(idx, f)
        n += 1
        if not args.quiet and n % 600 == 0:
            dt = time.time() - t0
            print("[%s] %d frames (%.0f f/s)" % (label, n, n / max(dt, 1e-9)),
                  flush=True)
    ctx = acc.finalize()
    ctx["decode_fps"] = n / max(time.time() - t0, 1e-9)
    print("[%s] decoded %d frames %dx%d @%.3gfps (analysis stride %d, "
          "%.0f f/s)" % (label, n, src.w, src.h, src.fps, ctx["s"],
                         ctx["decode_fps"]))
    return ctx, src


def main():
    ap = argparse.ArgumentParser(prog="autotest.analyze")
    ap.add_argument("source", help="video file or directory of PNG frames")
    ap.add_argument("--out", required=True, help="report directory")
    ap.add_argument("--baseline", default=None,
                    help="second video for A/B comparison")
    ap.add_argument("--fps", type=float, default=60.0,
                    help="fps for frames-dir input (videos use probed fps)")
    ap.add_argument("--maxw", type=int, default=640,
                    help="analysis-plane target width (default 640)")
    ap.add_argument("--dup-eps", type=float, default=0.15,
                    help="mean|diff| below which a frame is a duplicate")
    ap.add_argument("--flicker-eps", type=float, default=2.5,
                    help="per-channel delta (0-255) needed to count an "
                         "alternation; ~codec-noise floor")
    ap.add_argument("--suspects", type=int, default=30,
                    help="max suspect PNGs to extract")
    ap.add_argument("--keep-frames", default=None, metavar="DIR",
                    help="also dump the decoded frame sequence as PNGs")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ctx, src = run_pass(args.source, args,
                        keep_dir=args.keep_frames, label="main")
    if ctx["n"] < 8:
        sys.exit("[autotest] too few frames decoded (%d)" % ctx["n"])

    results = [d().run(ctx) for d in ALL_DETECTORS]

    baseline = None
    if args.baseline:
        bctx, _ = run_pass(args.baseline, args, label="baseline")
        baseline = {"name": args.baseline, "series": bctx["series"]}

    csv_path = os.path.join(args.out, "metrics.csv")
    write_metrics_csv(csv_path, ctx)
    heatmaps = write_heatmaps(args.out, ctx)
    chosen = pick_suspects(results, args.suspects)
    suspects = dump_suspects(args.out, args.source, ctx["fps"], chosen,
                             src.is_dir)
    write_events_json(os.path.join(args.out, "events.json"),
                      args.source, ctx, results)
    write_report(os.path.join(args.out, "report.md"), args.source, ctx,
                 results, heatmaps, suspects, baseline)

    # console summary
    print("\n== autotest verdicts ==")
    for r in results:
        print("  %-24s %s  (score %.3f, %d events)"
              % (r.name, r.verdict, r.score, len(r.events)))
    print("  wrote %s" % os.path.join(args.out, "report.md"))


if __name__ == "__main__":
    main()
