# M8b-live — REAL DLSS 5 live on the desktop via Arc Pro B50 (FINAL, 2026-09-20)

## Verdict: PASS — the full 71-block DLSSNR graph runs live on the desktop
Overlay presents the chain-processed frame at ~12.8 fps (2560x1440 capture,
288-token grid), verify metrics nonzero on frames 10/30/60, result PASS.

## Numbers (this final run)
- `--frames 40 --novideo` : verify frames 10,30 ALL PASS
  - frame 10: mean|final-native| B=26.81 G=30.44 R=20.20; |grad(delta)| B=1.58 G=1.73 R=1.42; changed 51.99%
  - frame 30: mean|final-native| B=26.99 G=31.05 R=23.38; |grad(delta)| B=1.62 G=1.75 R=1.43; changed 51.31%
  - avg 11.47 fps, 0 dropped
- `--frames 200` (live overlay, wiggle ON, blit STORAGE present path): ALL PASS
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
Launcher: C:\Users\AI\Desktop\RUN-DEMO.cmd (m8blive.exe) / RUN-DEMO-M4.cmd (m4).
