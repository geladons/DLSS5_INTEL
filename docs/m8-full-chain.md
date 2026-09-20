# M8a — Full 71-block DLSSNR chain on Arc B50

Status: BUILDING (started 2026-09-20 ~05:20 PDT). Target: Intel Arc Pro B50, Vulkan compute,
console app, synthetic input (NO DDA/present).

Spec: `docs/m6b-graph-design.md` (§A ops, §B weight map, §D executor). Rounding contract per
`docs/m7-proto.md` (M7 final: 100% bit-exact unit kernels; block31 validated 12/14 with two
documented accumulation-order exceptions). Reference semantics: `reference/dlss-nr-on-intel/src/ref/nr_model.py`.

## Key design decision (M8a scope interpretation)

The task brief specifies "the chain for a 288-token (24x12) grid at the design's channel
progression". Per the incremental rule and the brief's own NumPy-at-288-tokens note, M8a runs
**all 71 blocks at a fixed 24x12 = 288-token grid** (height=24, width=12), channels progressing
32 → 64 → 128 → 256 → 512 → 1024 → 512 → 256 → 128 → 64 → 32.

Consequences (documented divergences from the production geometry, all spatial-only; every
GEMM / rounding point / publish follows nr_model.py exactly):

- Downsample transitions (b4/8/14/22): `avgpool2` neutralized (would break 8-alignment
  immediately: 24x12 → 12x6 → …). Kept exactly: window block (publish=False) →
  `e4m3(e4m3(raw) @ weight0)`. b22's `pad_spatial_end(8)` also neutralized (12→16 would
  change token count).
- b30 bridge: pool-HALF neutralized; kept `e4m3(pad8(x)) @ layer4.weight` → e4m3
  (pad8 neutralized).
- Upsample transitions / b39: `nearest_upsample2_crop` neutralized; kept the projection GEMM,
  `+ skip * sin` merge, e4m3. Skip stack wiring identical to nr_model (skips[0..3] = outputs
  of b3/b7/b13/b21; split_skip = b30 output; full_res_skip = e4m3(b0 raw)).
- b70: merge `up*merge_sin + full_res_skip*merge_cos` then window block (no publish) then the
  two-GEMM head (GPU folds to one K=32 GEMM — documented reassociation).
- Window attention: real 8x8 windows with per-block shifted origins on the 24x12 grid
  ((0,0)→6 windows, (-4,-4)/(-4,0)→8, (0,-4)→6; worst case 8 windows = 512 padded tokens).
  Golden = nr_model.py fork with the same neutralizations (pool/upsample/pad8 → identity),
  `NR_FUSE_BRANCHED=1` (GPU uses the §A.7.2 fused fold — same bytes relaid at load).

Weight residency: all 649 tensors in ONE device-local VkBuffer per docs/pack-layout.txt
offsets (256-aligned, 291,535,872 B), load-time preprocessing in the staging memcpy
(§B.3): bias unswizzle for H∈{1,16}, branched-FFN fuse-fold (in place, same bytes).
Derived fp32 side tables (cos_skip/sin/merge vectors converted f16→fp32, global
attn_scale·√32, folded head weight [32,16]) live in the arena region after the pack.

## Kernels

Reuse from M7 (proven): gemm.comp (8x16x16 coopmat, all 5 epilogues), cosine.comp (global
layout), elementwise.comp (kinds 0-3), softmax.comp (extended: +optional window bias),
gemm compiled a second time with -DGEMM_RN=1 for the head GEMM (N=16). New: partition.comp
(image→windows, origin-aware zero-pad, f16/fp32 in), cosine_win.comp ([W,H,64,32] layout),
transpose_we.comp (merge_heads + e4m3), gather_residual.comp (reverse-window + residual +
optional publish), merge.comp (b39/up-transition merges + b70 pre-merge).

## Per-family verdicts

(M8a validation 2026-09-20 ~12:15 PDT — golden.py full-chain reference, all 71
blocks, 9 boundary tensors vs regenerated dumps. Full evidence:
`docs/m8-golden.log`.)

| family | blocks | status |
|---|---|---|
| weights residency (649 tensors, one buffer) | — | OK (offsets cross-checked) |
| stem (plain32 window + adapter + ds) | 0-4 | **FAIL at b1-b4 (shifted origins)**; b0 itself bit-exact through all 13 stages (maxabs ≤ 4e-6 at adapter/ffn/proj/scores/probs/ctx/merge/projGEMM/gather) |
| encoder (branched window 64/128/256 + ds) | 5-22 | FAIL (cascade; also shifted-origin + bias-swizzle class) |
| bottleneck (split512 window) | 23-30 | FAIL (cascade) |
| global (1024 MHA ±3 cap) | 31-38 | FAIL (cascade) |
| decoder (b39 merge + split + up-transitions + branched) | 39-69 | FAIL (cascade) |
| head (b70 + merge + head GEMMs) | 70 | FAIL (cascade; merged70/head fp32 mean-rel 1.0/0.25) |
| **FULL CHAIN (1402 dispatches)** | 0-70 | **RUNS 48.4 ms/frame; numerics FAIL vs golden** |

VALIDATION (2026-09-20): golden.py is a complete full-chain NumPy reference
(all families per §A, neutralized-geometry fork per this doc, `golden/*.npy`
per boundary). **Overall verdict: FAIL — root cause GPU-side, golden proven
correct on block 0.** Findings:

1. **Shipped dumps were garbage**: the 11:35 `gpu_*` dumps came from a
   `--to 0 --dispdbg` run (17 dispatches); everything past b0 was
   uninitialized arena. All compares below use full-chain-regenerated dumps.
   Any `--to N` run rewrites the 9 boundary dumps — full validation must use
   a no-args run.
2. **Block 0 (origin (0,0)) is numerically CORRECT end-to-end**: every stage
   bit-exact/near-exact vs the golden (scores/probs/ctx/merge/proj/gather all
   maxabs 0-4e-6). Every rounding point (half_round, e4m3, gate,
   cosine fragment tree, bit-affine softmax) confirmed in-place.
3. **Bias layout**: the running GPU consumes `attn_bias` in raw stored order
   (probs bit-exact with raw; device bytes == file bytes). nr_model/§A.6 say
   H∈{1,16} stored bias is fragment order and must be unswizzled at load.
   The unswizzle exists in committed main.cpp (with an M8_DEBUG_BIAS print)
   but demonstrably does not execute in the built exe (no print, raw device
   bytes, reproduced across two rebuilds incl. forced main.cpp recompile).
   golden.py defaults to raw (match the running chain);
   `M8_BIAS_MODE=swizzle` selects nr_model semantics. Either way b1+ diverge.
4. **Root cause of full-chain FAIL: shifted-window origins**. b0 (0,0) exact;
   b1-b4 use origins (-4,-4)/(0,-4)/(-4,0) whose zero-pad+crop path
   (partition.comp / gather_residual.comp) diverges: b4ds bitmatch 2.09%
   (raw) / 2.62% (swz) vs ≥99.9% required. All later boundaries cascade
   (b48/b69 at 0.0000% = fully decorrelated e4m3 publishes; fp32
   merged70/head mean-rel 1.0/0.25).
5. **Blockers to PASS**: (a) fix shifted-origin pad/crop in the GPU window
   kernels; (b) resolve the bias-unswizzle exe anomaly (rebuild clean and
   confirm [bias-swz] fires + device holds swizzled bytes); regen dumps from
   a no-args run; rerun `python golden.py <safetensors> build\Release\out`
     in both bias modes.

## Timings

(see table; timestamp-measured at 288 tokens @ 24x12 grid — directly
comparable to the live pipeline's 0.55 render_scale geometry. 53 ms beats the
101 ms projection — projection was conservative.)

## Extrapolation to M3 real geometry

(pending — per-family TFLOP/s × design doc §F.1 per-level FLOPs at 1408x768)
