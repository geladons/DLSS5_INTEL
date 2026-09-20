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

(pending — updated as each family validates)

| family | blocks | status |
|---|---|---|
| weights residency (649 tensors, one buffer) | — | BUILDING |
| stem (plain32 window + adapter + ds) | 0-4 | PENDING |
| encoder (branched window 64/128/256 + ds) | 5-22 | PENDING |
| bottleneck (split512 window) | 23-30 | PENDING |
| global (1024 MHA ±3 cap) | 31-38 | PENDING |
| decoder (b39 merge + split + up-transitions + branched) | 39-69 | PENDING |
| head (b70 + merge + head GEMMs) | 70 | PENDING |

## Timings

(pending)

## Extrapolation to M3 real geometry

(pending — per-family TFLOP/s × design doc §F.1 per-level FLOPs at 1408x768)
