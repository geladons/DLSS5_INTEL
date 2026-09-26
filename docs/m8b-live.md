# M8b-live — REAL DLSS 5 live on the desktop via Arc Pro B50 (FINAL, 2026-09-20)

## Verdict: PASS — the full 71-block DLSSNR graph runs live on the desktop
Overlay presents the chain-processed frame at ~12 fps (2560x1440 capture,
288-token grid), verify metrics nonzero on frames 10/30/60, result PASS.
Composite/format bug FIXED (no rainbow/overexposure, PNG-verified) and the
live loop is now EVENT-DRIVEN: zero GPU burn while the screen is static.

## Event-driven idle (user priority, 2026-09-20 final)
- Live loop blocks on `IDXGIOutputDuplication::AcquireNextFrame(1000 ms)`.
  Frame acquired -> process + present. Timeout -> NOTHING: no chain, no
  present, no readback, no submit (GPU ~0% on a static screen). Idle
  heartbeat every ~5 s: `[m8b] idle, waiting for updates (Ns ...)`.
- Per-frame timing line: `[frame] n processed in X ms (acq .. bridge .. rec ..
  gpu .. | fe .. fp .. chain .. hp .. tail ..)` via vkCmdWriteTimestamp
  (query pool, 6 slots) + CPU timers.
- Verified on static screen: alternating `acq 8 ms` (frame pending) and
  `acq 513-886 ms` (blocking waits, zero GPU work between) — 40 frames paced
  at ~1 Hz by the taskbar clock, wall 19.2 s, GPU idle throughout the gaps.
- Cursor wiggle REMOVED from the default path. `--wiggle-idle N` (default
  OFF) engages the gentle generator after N wall-clock seconds without any
  acquired frame; it now disengages only on REAL screen content (sample-diff
  discriminator: wiggle moves only the cursor ~<10 sparse samples, real
  changes hit >32) instead of pulsing off on its own frames.
- `--frames` counts PROCESSED frames.

## Perf regression 12.8 -> 4.2 fps: root cause = NO GPU regression
Per-stage timestamps (6-slot query pool) on the live loop, 40-frame runs:
- chain 47.9-58 ms (unchanged vs m8a 47), featpack ~0, headpack ~0,
  front-end 0.7-3 ms, tail (rescale-up+compose+encode) 1.2-2.3 ms,
  CPU bridge ~2 ms, command record ~1.3 ms. Suspects (a)-(d) all cleared:
  headpack/composite are sub-ms; rescale/features unchanged; no per-frame
  vkDeviceWaitIdle (only the fence); the [dbg] probe is frame==1-guarded.
- The 4.2 fps wall-clock was ARRIVAL-PACED, not GPU: with the always-on
  wiggle gone (user demand), a near-static screen (taskbar clock ~1 Hz)
  starves DDA; the event-driven loop correctly idles, so wall fps collapses
  while per-frame processing stays ~50-58 ms.
- Throughput with continuous content (external cursor activity, equivalent
  to a busy screen): `--frames 60 --novideo` = 11.83 fps avg / 12.0 rolling,
  every frame acq ~7-8 ms, chain ~50-51 ms, 0 drops — >=10 fps target met.
- Net: 4.2 (static-screen arrival pace) -> 11.8 fps (content-paced stream).

## Numbers (this final run)
- `--frames 40 --novideo` (event-driven, wiggle OFF): verify frames 10,30
  ALL PASS; frame 10 mean|d| B=3.95 G=4.65 R=3.40; frame 30 B=3.78 G=4.73
  R=3.35; changed ~91%; out\m8b_processed.bmp -> PNG INSPECTED: clean, no
  rainbow/red shift (subtle matched residual, mean|d| ~4-5 of 255).
- `--frames 60 --novideo` + continuous cursor activity (busy-screen equiv):
  11.83 fps avg / 12.0 rolling, 0 drops, chain ~50-51 ms steady.
- `--frames 40 --novideo` on static screen: 2.08 fps wall (arrival-paced by
  design), acq waits 513-886 ms with GPU idle, 0 drops.
- `--frames 200` (earlier, wiggle ON, blit STORAGE present path): ALL PASS,
  avg 12.78 fps — the pre-idle-loop baseline.
  - frame 10: mean|final-native| B=12.26 G=16.33 R=17.73; changed 39.89%
  - frame 30: mean|final-native| B= 9.10 G=14.65 R=12.62; changed 30.46%
  - frame 60: mean|final-native| B= 8.39 G=12.99 R=11.72; changed 27.47%
  - avg 12.78 fps (rolling 12.2-13.8), 0 dropped, wall 15.65 s
- Chain-internal frame-1 probe: MRG70 [-115.4, 118.2] (matches m8a golden
  distribution [-125.0, 109.1]); HEAD [-4.9, 58.3]; head4/headUp fully nonzero.
- BMP eyeball pair (frame 30): out\m8b_native.bmp vs out\m8b_processed.bmp —
  processed frame shows a clear global residual (color/structure delta).

## Root cause of the zero-delta bug (found + fixed today)
The composite was never the problem — the chain HEAD output was exactly 0.
Trace: featV nonzero -> x16 nonzero -> ADA/encoder/decoder boundaries all
healthy (B4DS/B22DS/B38/B48/B69 = +-448) -> MRG70 ~= 1e-10 -> f16 underflow
to 0 at the block-70 window input -> HEAD = 0 -> compose delta = 0.

`shaders/m8/merge.comp` had two compounding defects (present since the m8a
WIP executor, inherited verbatim):
1. kind-1 (block-70 pre-merge) converted f16 operands with
   `uint(float16_t)` = numeric truncation to integer, then re-widened those
   small integers as f16 bit patterns -> subnormals ~2^-24. +-448 became
   ~2.7e-5; products landed at ~1e-10 (MRG70 max was exactly 2^-31).
   Fix: `uint(float16BitsToUint16(...))` (bit reinterpret), as kind-0 does.
2. BOTH kinds read the sin/cos tables through `HalfBuf`, but the host
   (`placeVecF16`) stores those tables WIDENED TO FP32 in the arena. Every
   merge (b39/b48/b56/b62/b66/b70) therefore mixed a correct operand with a
   garbage-scale table. Fix: read tables via `FloatBuf` (fp32).
m8a's own saved artifacts (gpu_merged70.bin max 5.2e-10, gpu_head.bin all
zero) prove the b70 merge + head were NEVER golden-clean; the earlier
"full validation PASS" did not cover them. Same fix applied to
dlss5/m8-full-chain/shaders/merge.comp (m8a's copy) in this commit.

## Perf notes (perf pass is a later milestone)
- Chain ~47 ms steady-state in isolation; live loop end-to-end ~12.8 fps at
  2560x1440 including DDA + CPU bridge + front-end + compose + present.
- Zero DDA drops in the final runs (wiggle keeps dirty-rects flowing).
- One-shot frame-1 [dbg] probe (chain-buffer statistics) kept in main.cpp —
  prints once, no files.

## Run
```
dlss5\m8b-live\build.cmd                # configure + build (+ shaders)
dlss5\m8b-live\runm8b.cmd --frames 200  # live overlay (log -> docs\m8b-live.log)
build\Release\m8blive.exe --frames 40 --novideo   # headless verify
```
Launcher: C:\Users\<owner>\Desktop\RUN-DEMO.cmd (m8blive.exe) / RUN-DEMO-M4.cmd (m4).


## DARKNESS ROOT CAUSE + FIX (2026-09-20, final)

### Symptom
Live overlay structure correct but the whole screen rendered DARK
(~18%-brightness structured image); GDI screenshots and DDA dumps agreed the
screen was really dark; video-mode imgFinal readback dark too; fbmean stayed
healthy ~12; `--novideo` readbacks were BRIGHT (misleading - that path has no
overlay/feedback).

### Root cause: frame-0 safety-clamp lock-in (NOT alpha, NOT sRGB)
All three alpha suspects were checked and are clean: imgFinal/readback alpha
is 255 everywhere (BMP byte check), swapchain format=44 =
VK_FORMAT_B8G8R8A8_UNORM (no sRGB in play), compositeAlpha=OPAQUE was already
chosen (supported=0x9) and is now forced + logged. The real mechanism is the
feedback-cancellation seed:
- bufLastPresented starts ZEROED. Frame 0's fbcancel computes
  `corrected = clamp(capture - 0, +/-4*maxDelta)` = `min(desktop, 48)/255`
  per channel: a structured image capped at ~18% brightness.
- encode presents it; bufLastPresented becomes that dark frame.
- From frame 1 on, `corrected = clamp(capture - lastPresented, +/-48)` and
  the capture IS the dark presented frame, so corrected ~= 0 and the loop
  STABILIZES on the dark frame forever (stable fixed point: presented dark ->
  capture dark -> delta ~ 0 -> stays dark). fbmean ~12 is just the wiggle
  cursor echo (cursor is in the DDA capture but not in our presented frame).
- The same clamp bug also explains why it could never self-correct: the only
  "escape" signal (capture vs lastPresented difference) is ~0 by construction.

### Fix (minimal)
- `shaders/fbcancel.comp`: new SEED mode (push c.x=1, set for frame==0 only in
  main.cpp): pass the capture UNclamped when lastPresented is still all-zero -
  the "delta" IS the whole desktop at frame 0 and must seed the accumulation,
  not be strangled by the transient safety bound. Normal frames unchanged.
- Hardening (correctness, not the bug): overlay window is no longer
  WS_EX_LAYERED (LWA + swapchain goes through DWM alpha paths) - plain
  TOPMOST|TRANSPARENT popup; compositeAlpha forced OPAQUE with a loud warning
  if unsupported; encode.comp writes A=1.0 explicitly (presented overlay is
  opaque by construction).

### Evidence (screen-verified)
- Live run `--frames 60 --wiggle-idle 3`: frame 0 fbmean 40.8 -> 13.4 flat
  across frames 9-14 (bounded, no divergence); verify frame 10 metrics PASS
  (mean|final-native| B=0.458 G=0.123 R=0.502, structure PASS, changed 65%).
- REAL screen screenshot (ui.ps1): BRIGHT desktop - vivid wallpaper, light
  taskbar, readable windows, no global darkening/tint (out\_seedfix_live.png).
- DDA ground truth of the displayed screen (out\_seedfix_dda2.png): BRIGHT -
  the displayed image is truly bright, not a capture artifact.
- Readback == screenshot == DDA, all bright: present path faithful.
- Idle: 6300+ settle-skips at est mean|d|=0.000, zero GPU submissions while
  static (event-driven idle intact); fbmean flat 13.38-13.47 over all frames.

### Known follow-up (not the darkness bug)
Fullscreen feedback can never observe the TRUE desktop (opaque overlay covers
it; DDA sees only our own last frame + cursor). When frames process
continuously, the network's correlated hue residual integrates at up to
+/-12/255 per processed frame (visible as the mild wallpaper hue shift) - a
design limitation of fullscreen self-feedback, gated in practice by the
settle-skip (drift only accrues while real activity processes frames).
