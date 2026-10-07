# autotest - automated game capture + quality-analysis rig

Two-stage pipeline that records the REAL game window at high frame rate and
then dissects the video with a battery of artifact detectors. Built because
log-based verification cannot see what a user sees: the layer can be serving
processed frames while the visible output still flashes, flickers or stalls.

```
capture_session.py -> game launch + daemon + layer + DDA record -> capture.mp4 + manifest.json
analyze.py         -> decode + 7 detectors -> report.md + events.json + metrics.csv
                      + suspects/*.png + heatmaps (+ optional --baseline A/B table)
synth_fixture.py   -> synthesises one injected artifact per detector; self-tests
                      the battery without a game
```

NumPy + stdlib only (cv2 optional, not required). Decodes via the repo's
ffmpeg binary.

## Capture (capture_session.py)

    # animated loading screens + menu (window appears ~20 s in)
    python tools/autotest/capture_session.py --mode menu --dur 60 --fps 120 --out Temp/run1

    # let the game load 150 s (daemon roughly doubles load vs clean ~75 s),
    # then film the main menu 45 s
    python tools/autotest/capture_session.py --mode menu --dur 45 --pre-roll 150 --out Temp/menu2

    # flythrough: benchmark self-runs a camera flight - continuous motion,
    # the hardest temporal-coherence test. The window DIES ~124 s post-launch
    # (flythrough end -> results/exit); film BEFORE it does:
    python tools/autotest/capture_session.py --mode benchmark --dur 60 --pre-roll 55 --out Temp/bench1

    # control run without layer/daemon (native reference)
    python tools/autotest/capture_session.py --mode menu --dur 30 --native --out Temp/native1

Important flags:

- `--pre-roll S` - game runs S seconds with the window already defended,
  recording starts only after. Reaches the menu/flythrough without burning
  tens of GB on the loading phase. Window rect is re-anchored after the wait.
- `--native` - strips all layer env vars, implies `--no-daemon`. A/B baseline.
- `--no-window-mgmt` - zero window manipulation (isolation test variable).
- `--fg {0,1,2}` - framegen mode pushed to daemon and layer knobs.
- `--env KEY=VAL` - extra env for the layer (e.g. `NR_LAYER_RESIDUAL=1`).
- `--keep-raw` - keep the multi-GB BGRA raw next to the mp4 (else deleted
  after encode). Raw is `W*H*4*fps` per second: 816x638x120 = ~63 MB/s.

The harness writes GTA IV `commandline.txt` for the mode and restores it
byte-for-byte afterwards; kills only processes it spawned; never touches a
pre-existing daemon or Sunshine.

### manifest.json

mode, all args, window rect, requested fps/duration, PIDs, env vars, phase
timestamps (`t_window_seen`, `t_preroll_end`, `t_rec_start`...), daemon spawn
flags, ddarec/ffmpeg return codes, ddarec stderr stats
(emitted/repeated/acquired/timeouts), output paths.

**Trust ddarec stats, not nominal fps**: `emitted` is the real mp4 frame
count; a busy desktop may yield far fewer than requested. The mp4 is a
TIME-COMPRESSED view of the wall-clock window - events.json times are mp4
seconds, not wall seconds.

## Analysis (analyze.py)

    python tools/autotest/analyze.py <video.mp4|frames_dir> --out <reportdir>
                          [--baseline <other.mp4>] [--fps 60]
                          [--maxw 640] [--suspects 30] [--keep-frames DIR]

- `video.mp4` - any container ffmpeg can decode (probed fps used).
- `frames_dir` - directory of PNGs; natural-sorted order, `--fps` sets the
  timeline.
- `--baseline` - second pass over a reference clip (e.g. the --native run);
  appends a median/p95 comparison table to report.md.
- `--maxw` - analysis-plane width; video is downscaled for temporal metrics
  (suspect PNGs stay full-res).
- `--dup-eps` / `--flicker-eps` - duplicate and flicker thresholds.

Outputs in `<reportdir>`:

- `report.md` - verdict table (PASS/WARN/FAIL), notes, top events, hotspot
  bounding boxes, baseline table.
- `metrics.csv` - per-frame scalar timeline (luma/chroma means, dup flag,
  hist_dist, diff_mean, edge_energy, flicker_*, patch_idx, ghost_echo,
  static_var, updates_per_s).
- `events.json` - machine-readable verdicts + event ranges (seconds + frames)
  with evidence fields.
- `heatmap_flicker.png`, `heatmap_patchwork.png`, `heatmap_static_noise.png`.
- `suspects/` - full-res PNGs at worst event peaks (adjacent pairs for
  flicker). Eyeball these FIRST - detectors triage, they do not convict.

## Detectors

| name | signature |
|---|---|
| update-rate | distinct updates/sec collapse (slideshow, held-frame stalls); min event ~0.75 s |
| garland | sign-alternating per-pixel deltas across >=3 consecutive frames (up/down/up); channel-sign disagreement = colourful flicker; flicker-fraction timeline + hotspot boxes |
| luma-chroma-pumping | windowed oscillation in global/regional luma and Cb/Cr means + one-frame flash spikes |
| spatial-patchwork | dense interleave of held (delta~0) and live (delta>4) 8x8 blocks in the frame-delta map = checkerboarded raw/enhanced mixing |
| ghost-double-contour | pixels held >=8 frames inside a moving, edged 32x32 block (latched smear) + lagged edge echo; chronic-hold fraction reported for FG-heavy footage |
| scene-change-staleness | histogram-distance cut followed by a >=0.5 s freeze with motion before/after; tolerant of encoder settling after cuts |
| static-region-noise | pixels near their running mean but with high temporal std (jittering menu/loading chrome) |

Verdicts: PASS = nothing, WARN = worth a look, FAIL = inspect suspects.

## Always run the native control

Absolute detector thresholds are content-dependent: a fast night-city
flythrough trips `garland` on PURE NATIVE footage (real motion = real
per-frame deltas). The correct verdict is RELATIVE - same mode, same timing,
`--native` capture vs layered capture, compared per-detector or via
`--baseline`. Measured on this host (benchmark mode, older metric):

| metric | native | layer+daemon | meaning |
|---|---|---|---|
| spatial-patchwork events | 1 | 38 | layer mixes stale/fresh regions |
| update-rate median | ~34/s | ~4/s | async layer slashes visible updates |
| ghost events | 0 | 2 | stale-blend echoes are layer-only |
| garland score | ~0.69 | ~0.78 | dominated by scene motion - use static captures |

The `static-region-noise` verdict may INVERT (native FAIL, layered PASS):
the enhancer eats GTA IV's film grain - a feature, not a bug.

## Self-test without a game

    python tools/autotest/synth_fixture.py Temp/synth.mp4
    python tools/autotest/analyze.py Temp/synth.mp4 --out Temp/synth_analysis

Every detector should fire on its injected artifact - regression-gates the
battery itself.

## Host facts discovered by this rig (do not relearn)

- GTA IV ignores ALL injected input (DInput) - menus cannot be clicked
  through synthetically. `-benchmark` is the only deterministic gameplay
  route.
- Clean launch -> main menu in ~75 s. With layer+daemon the load is ~2x
  slower (~150 s); the VISIBLE window can sit on the legal screen while the
  game renders internally - plan `--pre-roll` accordingly.
- Benchmark mode: window dies at ~124 s post-launch (flythrough end ->
  results/exit), process stays alive briefly. `--pre-roll 55 --dur 60`
  covers the ~90-124 s flythrough; pre-roll 160 films only a dead desktop.
- DDA records the composited desktop: a covered game window records whatever
  covers it. The harness minimizes intersecting occluders and restores them;
  the keeper logs `[win] LOST` / `re-appeared` so a dead-window capture is
  diagnosable instead of yielding silent desktop footage.
- The first DDA frame is a black warm-up frame (skipped automatically).
- Static content = mostly identical frames + rare single-frame anomalies
  (parallel raw presents bypassing the hook, paint glitches). The garland
  lives where content changes every frame - benchmark mode is the stress test.
