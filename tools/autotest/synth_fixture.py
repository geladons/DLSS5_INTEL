#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# synth_fixture - synthesise a validation video with one injected artifact
# per detector, so the rig can be regression-tested without a game.
#
#   python synth_fixture.py <out.mp4> [--w 640 --h 360 --fps 60]
#
# Timeline (60fps defaults):
#   0.0-2.0s  normal motion (panning texture + moving box)
#   2.0-3.0s  GARLAND: 180x110 rect toggles +-30 per frame, channel-split
#   3.0-4.0s  PUMPING: global luma sine +-7 at ~2 Hz
#   4.0-5.0s  STALENESS: hard cut to a new scene, then 0.9s exact freeze
#   5.0-6.0s  GHOST: fast box leaves a 35%-opacity contour at its t-3 pos
#   6.0-7.0s  PATCHWORK: region with alternating 8px noisy/clean blocks
#             whose delta pattern checkerboards
#   7.0-7.2s  FLASH: one-frame +60 luma spike
#   7.2-9.0s  SLIDESHOW: content updates only every 10th frame
#   9.0-11.0s STATIC-NOISE: frozen background, one rect keeps +-18 jitter
# ============================================================================
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import write_png                     # noqa: E402
from videoin import find_ffmpeg                 # noqa: E402


def base_frame(i, w, h, rng):
    """Deterministic moving scene: gradient + panning sin texture + box.
    The pan shifts the high-frequency texture 6px/frame so |d1| clears
    the codec floor wherever content truly moves - like real footage,
    where uniform pans still leave >2-unit deltas on any texture."""
    y, x = np.mgrid[0:h, 0:w]
    g = ((x * 0.35 + y * 0.2 + i * 1.7) % 160 + 40).astype(np.float32)
    tex = (35 * np.sin((x + i * 6) * 0.25)
           * np.cos((y - i * 2) * 0.21))
    grain = rng.randint(-1, 2, (h, w)).astype(np.float32)
    f = np.clip(g + tex + grain, 0, 255)
    # moving bright box, 60x40, speed 5px/frame diagonal
    bx = int((i * 5) % max(1, w - 80))
    by = int((i * 3) % max(1, h - 60))
    f[by:by + 40, bx:bx + 60] = 235
    rgb = np.stack([f, np.clip(f * 0.9 + 10, 0, 255),
                    np.clip(255 - f, 0, 255)], axis=-1)
    return rgb.astype(np.uint8), (bx, by)


def gen(out, w, h, fps):
    rng = np.random.RandomState(7)
    n = int(11 * fps)
    tmp = tempfile.mkdtemp(prefix="synth_")
    frozen = None
    scene_b = None
    frames_since_update = 0
    slide = None
    ghost_patch = None
    prev_rgb = None
    for i in range(n):
        t = i / fps
        rgb, (bx, by) = base_frame(i, w, h, rng)
        if 2.0 <= t < 3.0:                       # garland
            ph = i % 2
            reg = rgb[80:190, 300:480].astype(np.int16)
            amp = np.where(ph, 30, -30)
            reg[..., 0] += amp                   # B channel
            reg[..., 2] -= amp                   # R opposite = colourful
            reg[..., 1] += (amp * 0.4).astype(np.int16)
            rgb[80:190, 300:480] = np.clip(reg, 0, 255)
        elif 3.0 <= t < 4.0:                     # pumping
            rgb = np.clip(rgb.astype(np.float32)
                          + 7 * np.sin(2 * np.pi * 2.0 * t), 0, 255
                          ).astype(np.uint8)
        elif 4.0 <= t < 5.0:                     # cut + freeze
            if scene_b is None:
                scene_b = np.clip(
                    base_frame(i, w, h, rng)[0].astype(np.int16) + 60,
                    0, 255).astype(np.uint8)
                frozen = scene_b
            rgb = frozen if t < 4.9 else base_frame(i, w, h, rng)[0]
        elif 5.0 <= t < 6.0:                     # ghost: held smear patch
            if ghost_patch is None:
                # freeze a copy of the scene at the box's current pos;
                # the scene pans on but this rect stays byte-identical
                ghost_patch = (rgb[by:by + 40, bx:bx + 60].copy(),
                               (bx, by))
            px, py = ghost_patch[1]
            rgb[py:py + 40, px:px + 60] = ghost_patch[0]
        elif 6.0 <= t < 7.0:                     # patchwork
            # alternating 16px blocks on a 16px grid boundary: half update
            # normally, half hold the previous frame verbatim -> the
            # DELTA map checkerboards, the raw/enhanced-mixing signature.
            # (must align with the detector's 8s*8px block grid or the
            # contrast dilutes into every block)
            if prev_rgb is not None:
                blk = np.kron((np.indices((8, 10)).sum(0) % 2) * 2 - 1,
                              np.ones((16, 16)))
                reg = rgb[192:320, 96:256]
                hold = prev_rgb[192:320, 96:256]
                rgb[192:320, 96:256] = np.where(
                    (blk < 0)[..., None], hold, reg)
        elif 7.0 <= t < 7.2 and int(t * fps) == int(7.1 * fps):
            rgb = np.clip(rgb.astype(np.int16) + 60, 0, 255
                          ).astype(np.uint8)     # flash
        elif 7.2 <= t < 9.0:                     # slideshow
            if frames_since_update % 10 == 0 or slide is None:
                slide = rgb
            rgb = slide
            frames_since_update += 1
        elif 9.0 <= t:                           # static noise
            if i == int(9.0 * fps):
                frozen = rgb
            rgb = frozen.copy()
            nz = rng.randint(-18, 19, (60, 90, 1), np.int16)
            rgb[240:300, 400:490] = np.clip(
                rgb[240:300, 400:490].astype(np.int16) + nz, 0, 255
                ).astype(np.uint8)
        write_png(os.path.join(tmp, "f%05d.png" % i), rgb[..., ::-1])
        prev_rgb = rgb.copy()
        # rgb[] is r,g,b order in base_frame; write_png expects raw array,
        # and ffmpeg image2 decodes PNG as rgb24 - keep channel order as
        # authored (detectors are colour-agnostic for the artifact shapes).
    ff = find_ffmpeg()
    cmd = [ff, "-y", "-hide_banner", "-loglevel", "error",
           "-framerate", str(fps), "-i", os.path.join(tmp, "f%05d.png"),
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ffmpeg encode failed: %s" % r.stderr[-500:])
    print("wrote %s (%d frames %dx%d @%dfps)" % (out, n, w, h, fps))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=360)
    ap.add_argument("--fps", type=int, default=60)
    a = ap.parse_args()
    gen(a.out, a.w, a.h, a.fps)


if __name__ == "__main__":
    main()
