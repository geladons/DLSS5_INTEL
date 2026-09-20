# M6b — Vulkan Compute Executor for Full DLSSNR Transformer Graph (71 blocks)

Status: DESIGN-ONLY. Date: 2026-09-20. Target: Intel Arc Pro B50, Vulkan compute, 1408x768 extent (M3 render_scale 0.55), FP16/f16 coopmat (8x16x16).

Sources:
- reference/dlss-nr-on-intel (daemon/graph/GLSL/xmxres.py; notes/phase3-weight-format.md; phase23-integer-weights.md)
- work/mlx-dlss/python/mlxdlss (PyTorch/MLX reference)
- docs/weights-inventory.txt; PROGRESS.md (M3 + M6a); docs/analysis-dlss-nr-on-intel.md

Authoritative graph spec: `reference/dlss-nr-on-intel/src/ref/nr_model.py` (numpy reference, docstring: faithful port of `work/mlx-dlss/python/mlxdlss/model.py`). GPU pass-fusion cross-check: `reference/dlss-nr-on-intel/src/gpu/nr_resident.py`, `xmxres.py`, `src/gpu/publish.glsl`, `docs/ARCHITECTURE.md`.

> **Correction to PROGRESS.md (M6a) and the M6b task brief:** the graph has **NO grouped-query attention**. `docs/ARCHITECTURE.md` §3 explicitly withdraws the early GQA reading: `qkv_weight` is `(C, 3C)` — full multi-head attention, head_dim 32, H = C/32 heads, per-head FP32 `attn_scale`. All sections below use full MHA.
>
> **Second correction:** the branched FFN (called "MoE" in the brief) has **no routing softmax and no top-k**. It is a dense branched expansion (§A.6). Do not implement a router.

---

## A. Op inventory

Notation: activations are carried between ops in FP32. GEMM inputs are converted to F16 (`half_round`) at GEMM boundaries unless an E4M3 publish is specified. Every rounding point below is mandatory (chaotic graph, ~100 E4M3 publishes — `docs/ARCHITECTURE.md` §5); matching `publish.glsl` exactly is the contract.

### A.1 Precision primitives (from `src/gpu/publish.glsl`, mirrored in `nr_model.py`)

- `half_round(x)` = FP16 round-to-nearest-even of x, carried back in FP32. Implementation MUST go through `packHalf2x16`/`unpackHalf2x16` (GLSL) — a plain `float(float16_t(x))` is folded away by the compiler (comment `publish.glsl:10-16`). In HLSL/our GLSL: same trick.
- `e4m3(x)`: saturate `m = min(|x|, 448)`; if `m < 2^-6` step = `2^-9` (subnormal) else step = `2^(exp(m)-3)` (i.e. 8 codes per binade, mantissa quantum 2^(e-3)); `roundEven(m/step)*step`; reapply sign. (`publish.glsl:19-29`, `nr_model._e4m3_chunk`.)
- `gate_activation(x)` — the FFN nonlinearity ("quadratic gate"), `precise` (no FMA contraction across rounding points):
  ```
  wide     = half_round(x)
  clamped  = clamp(wide, -4.0, 4.0)
  linear   = half_round(|clamped| * -0.055908203125 + 0.447265625)
  linear   = half_round(linear * clamped + 0.89453125)
  return   half_round(wide * linear)
  ```
  (`publish.glsl:31-41`, `nr_model._gate_wide`/`quadratic_gate_activation`.)
- Block-internal epilogue enum (bits 8-11 of pass flags in the reference runtime; `xmxres.EPI_*`): NONE=raw fp32, HALF=`half_round`, E4M3=`e4m3`, GATE=`gate_activation`, GATE_E4M3=`e4m3(gate_activation(x))`. Use fused epilogues on GEMM outputs wherever listed — same math, one pass less.

### A.2 Input contract (already implemented in M3 `features.comp`; listed for completeness)

16 FP32 channels (`docs/ARCHITECTURE.md` §2): 0-2 deterministic noise (`mlxdlss/features.py:deterministic_noise(height,width,frame_index)` — Gaussian pair from hash of pixel coords + frame index, rounded to FP16; the real function lives in vendored mlx-dlss, NOT in the reference clone), 3 const 1, 4-6 current frame `scaled(x)=half(half(half(x)-0.5)*0.125)`, 7-9 previous output (first frame repeats 4-6), 10-14 style/tone/structure/skin/auto-mask scalars (or per-pixel control mask), 15 zero. Extent rule ≥320 and multiple of 64; mirror-pad in, crop back (1408x768 already complies: both multiples of 64, ≥320).

### A.3 Stem (block0 front half)

`value = tokens @ input_adapter_weight` — a plain GEMM `[tokens,16]@[16,32]`, no publish. (Despite "stem/conv" in the brief, there is NO spatial conv in the stem; the only conv-named weights are `block39.layer0.conv_weight` (a 512←1024 projection GEMM) and `block70.layer0.out_conv_weight` (head GEMM) — both 1x1-equivalent matmuls, no stride/padding.)

### A.4 Window partition / shifted windows

8x8 windows = 64 tokens (`window_size=8` everywhere). `partition_windows` = reshape NHWC→(n,8,8,C)→windows `(nw,64,C)`; `reverse_windows` inverse. Shifted-window origins per block (`nr_model.recovered_window_origin`): phases cycle `[(0,0),(-4,-4),(0,-4),(-4,0)]` (i.e. `(dy,dx)` with `((0,-4,0,-4)[p%4],(0,-4,-4,0)[p%4])`):
- b0: (0,0). b1-4: phase = idx-1 (b1 (0,0), b2 (-4,-4), b3 (0,-4), b4 (-4,0)).
- b5-8: idx-5; b9-14: idx-9; b15-22: idx-15; b23-30: idx-23; b40-47: idx-40; b48-55: idx-48.
- b56-61: idx-54 (b56 (0,-4), b57 (-4,0), b58 (0,0), b59 (-4,-4), b60 (0,-4), b61 (-4,0)).
- b62-65: idx-62; b66-69: idx-66. b70: phase 1 = (-4,-4).
Negative origin → zero-pad that edge to a window multiple (`np.pad` constant 0; `window_attention` pads top/left = -origin, bottom/right = alignment), crop back after. Window count follows the block's own origin.

### A.5 Cosine normalize / publish (exact fragment tree, head_dim=32)

`vendor_cosine_normalize` (`nr_model.py`): input half_rounded first. Per 32-dim vector, with all arithmetic in FP16 (each op individually half-rounded; vendor lets overflow saturate to inf):
```
for lane in 0..3, parity in 0..1:  ch = lane*2 + parity
  first  = half_fma(x[ch+8],  x[ch+8],  half_mult(x[ch],   x[ch]))     # fma rounds once
  second = half_fma(x[ch+24], x[ch+24], half_mult(x[ch+16],x[ch+16]))
  partial[lane][parity] = half_add(first, second)
xor_two[lane] = half_add(partial[lane], partial[lane^2])   (4 lanes)
xor_one[lane] = half_add(xor_two[lane], xor_two[lane^1])   (4 lanes)
norm = half_add(xor_one[0][0], xor_one[0][1])  widened to fp32
norm = max(norm, 2^-14)                        # COSINE_NORM_FLOOR = 0.00006198883056640625
recip = half_round(1/sqrt(norm))               # fp32 rsqrt, ONE half rounding
return half_round(x * recip)
```
`cosine_publish(x, scale)`: normalize; if scale given (Q only): `half_round(normalized * half_round(scale))`; then `e4m3(...)`. (`nr_model.vendor_cosine_publish`.) Non-32 head_dim variant exists in the reference but is unreachable here (head_dim is 32 at every width).

### A.6 Attention (full MHA, head_dim 32)

`cosine_attention` (`nr_model.py`); per-batch-of-windows form:
1. `proj = tokens @ qkv_weight` (fp32 carrier; GEMM input f16). Split into Q,K,V along last axis; reshape `(batch, tokens, H, 32)`, transpose to `(batch, H, tokens, 32)`.
2. `Q = cosine_publish(Q, attn_scale)`; `K = cosine_publish(K)` (no scale); `V = e4m3(V)`.
   - Global blocks (31-38): effective scale = `attn_scale * sqrt(32)` computed in FP32 first (`nr_model.global_block`); window blocks use `attn_scale` as stored.
3. `scores = Q @ K^T` per head — **K is consumed transposed; the coopmat column-major load gives the transpose for free** (`matmul_nt`, `xmx.py:175-225`). FP32 accumulate.
4. Window blocks: `scores += attn_bias[H,64,64]` (broadcast per head); NO logit cap. Global blocks: no bias; `scores = clamp(scores, -3.0, +3.0)` (`GLOBAL_ATTENTION_LOGIT_CAP=3.0`, symmetric).
   - **Storage trap**: for head counts H∈{1,16} the stored bias is in NVIDIA mma-fragment order — apply `recover_attention_bias_layout` (`nr_model._fragment_swizzle_indices`) ONCE at weight-load; H∈{2,4,8} are stored logical. (`uses_fragment_swizzle`: `head_count in (1,16)`.)
5. `probs = vendor_approximate_softmax(scores)` → **output is E4M3** (f16 stored). Exact chain:
   ```
   a = half_round(score);  a = a * 0.044921875 + 1.30078125      (fp32 math)
   a = clamp(a, 1.03125, 1.5693359375)                            (fp32)
   bits = f16_bits(a) packed 2-per-u32:  packed = lo | (hi << 16)
   transformed = (packed << 5) + 0x7FF88000                       (u32 bit-affine exp approx)
   weights = f16(transformed split lo/hi 16-bit)                  (two f16 from one u32)
   totals  = sum(weights, per row)                                # fp32 accumulate, round ONCE to f16
   recip   = half_round(1 / fp32(totals))
   probs   = e4m3(half_round(fp32(weights) * fp32(recip)))
   ```
   Even token count required (64 per window ✓; global rows = padded token count — pad to even).
6. `context = probs @ V` per head `(64,64)@(64,32)`; merge heads → `(tokens, C)`; **`attended = e4m3(merged)`** (publish BEFORE projection).
7. `attention_branch = attended @ projection_weight` (fp32 carrier). Residual applied by the block wrapper (§A.8) — the projection output itself is NOT published.

Softmax numerics are the make-or-break op (reference: "softmax + cosine publish are arithmetic at ~45% of bandwidth", `notes/phase45`). Implement as a dedicated compute pass over score rows; 64-wide rows for window attention, up-to-padded-token rows for global (288 at 1408x768).

### A.7 Feed-forward families (3 kinds + global)

All FFNs: `branch = <expand path>`; block keeps `ffn_out = branch + tokens * ffn_cos_skip` (raw FP32 vector multiply — the cos_skip weight is used AS STORED, no normalize; confirmed both in `nr_model.cosine_residual` and GPU `runtime.residual(..., w.ffn_cos, ...)`).

1. **Plain (C=32; blocks 0-4, 62-70):**
   `branch = e4m3(gate(x @ weight1)) @ weight2`, with `EPI_GATE_E4M3` fused on the first GEMM. Shapes: weight1 `[C,4C]`, weight2 `[4C,C]`.
2. **Branched (C=64/128/256, H=C/32; blocks 5-22, 48-55):** dense, NO router:
   ```
   input_heads = split(x, H groups of 32)             # G = H = C/32
   per output head oh in 0..G-1:
     per branch br in 0..3:
       expanded = sum_ih (input_heads[ih] @ expand_w[oh,br,ih])    # (32,32) GEMMs
       branch_br = e4m3(gate(expanded)) @ branch_proj_w[oh,br]     # EPI_GATE_E4M3 then GEMM, E4M3 out
     head_out[oh] = e4m3(branch_0 + branch_1 + branch_2 + branch_3)
   ffn = concat(head_out) @ output_projection_weight               # (C,C) GEMM, NO e4m3 on output
   ```
   Weight shapes: `ffn_expand_weight [G,4,G,32,32]`, `ffn_branch_projection_weight [G,4,32,32]`, `ffn_output_projection_weight [C,C]`.
   **Fused form (recommended)**: fold to two GEMMs per head — expansion `(G*32, 128)` per oh via `transpose(0,2,3,1,4).reshape(G, G*32, 128)`, projection `(128, 32)` via reshape; gate+e4m3 pass through the block structure untouched (`nr_model._fused_branched_weights`, comment: reassociation differs by ~1e-7 — acceptable; we validate against fused since it is 1+G+1 GEMMs instead of 4G²+4G).
3. **Split-group (C=512, 8 groups; blocks 23-30, 40-47):**
   ```
   hidden = e4m3(x @ first_projection_weight)            # (C,C), EPI_E4M3
   per group g of 64 channels:  g_out = gate(g_hidden @ group_expand_w[g]) @ group_project_w[g]
                                # gate between the two GEMMs, NO e4m3 on the gate output
   core   = e4m3(concat(g_out))                          # one publish after concat
   branch = core @ weight3                                # (C,C), no e4m3
   ```
   Shapes: `first_projection_weight [512,512]`, `group_expand_weight [8,64,256]`, `group_project_weight [8,256,64]`, `weight3 [512,512]`.
4. **Global (C=1024; blocks 31-38):** plain FFN at width: `branch = e4m3(gate(x @ weight[1024,4096])) @ weight[4096,1024]` (EPI_GATE_E4M3), then residual. No windowing — tokens are the full 24x12=288 pixel grid; attention over all tokens per head (H=32).

### A.8 Residuals, block wrapper, publishes

Window/split block wrapper (`window_block`/`split_window_block`/`branched_window_block`):
```
ffn_out      = ffn(x)                                  # raw fp32, NO publish  ← NOTE
attn_branch  = attention(ffn_out)                      # reads ffn_out half-rounded at partition
block_output = attn_branch + ffn_out * attn_cos_skip   # raw fp32 vector gate
publish: block_output = e4m3(block_output)             # ALL blocks except b0-internal and b70
```
The GPU reference fuses the closing publish into the closing residual (`record_block(..., epilogue=publish)`); the FFN-internal residual of branched blocks publishes E4M3 (`record_feed_forward` branched path) — but that buffer is only read by the qkv partition, which half-rounds; in the numpy spec the FFN output is raw and only ONE publish exists per block output. **We follow the numpy spec** (single e4m3 per block output, raw fp32 residual buffers). This is a deliberate, documented divergence from the Linux GPU fusion (it removes ~70 extra e4m3 passes; per-element agreement with the numpy/PyTorch reference is what we validate).

### A.9 Level transitions and head

- **Downsample (blocks 4, 8, 14, 22)**: `out = e4m3(e4m3(avgpool2(block_raw)) @ weight0)`. `avgpool2` = 2x2 mean (FP32 sum * 0.25). Block 22 pads its input to a multiple of 8 first (`pad_spatial_end(8)`); 1408x768 chain reaches b22 at 88x48 — already 8-aligned, pad is a no-op at our extent but must exist for other extents.
- **Block 30 bridge**: `out = e4m3(avgpool2(pad8(x)) @ block30.layer4.weight)` — pool published HALF-only (no e4m3 before the GEMM), single e4m3 after.
- **Block 39 (bottleneck→decoder)**: `out = e4m3(nearest_up2(x @ conv_weight[1024,512]) + split_skip * inp_upsample_sin)`.
- **Upsample-transition blocks (48, 56, 62, 66)**: `out = window_block(e4m3(nearest_up2(x @ weight0) + skip * sin))` — project (no publish) → nearest 2x (repeat, crop to skip extent) → add sine-scaled skip → e4m3 → window block.
- **Final merge + head**: nearest_up2 b69 output to full res; `merged = up * inp_merge_sin + full_res_skip * inp_merge_cos` (raw FP32); block70 window block (H=1) on merged, NO closing publish; `head = tokens[...,:16] @ out_gain[16,4] + tokens[...,16:] @ out_conv[16,4]` (raw FP32 4-ch output).
- **Composition (already in M3 `compose.comp`, `nr_image.c:53-76`)**: `predicted = clamp(colour + half(head.rgb)*0.25, 0,1)`; `alpha = clamp(sigmoid(half(head.a)) * half(0.73974609375), 0,1)`; `output = predicted + alpha*(history - predicted)`; history = channels*8 + 0.5.
- **Skip stack**: `full_res_skip = e4m3(b0_raw)` BEFORE pooling; encoder skips pushed after blocks 3(→b4 input), 7, 13, 21 (`skips=[b3out, b7out, b13out, b21out]`) — i.e. skips[0..3] consumed by upsample transitions 66/62/56/48 respectively; `split_skip` = b30 output (pre-downsample) consumed by block39.

### A.10 Per-block dispatch table (1408x768, render_scale 0.55)

| blocks | type | C | H | tokens (px) | FFN kind | notes |
|---|---|---|---|---|---|---|
| 0 | window | 32 | 1 | 1,081,344 (1408x768) | plain | stem GEMM 16→32 before; output feeds skip + pool |
| 1-3 | window | 32 | 1 | 270,336 (704x384) | plain | |
| 4 | window+ds | 32→64 | 1 | 270,336 | plain | publish=False, pool, weight0[32,64] |
| 5-7 | window | 64 | 2 | 67,584 (352x192) | branched | |
| 8 | window+ds | 64→128 | 2 | 67,584 | branched | weight0[64,128] |
| 9-13 | window | 128 | 4 | 16,896 (176x96) | branched | |
| 14 | window+ds | 128→256 | 4 | 16,896 | branched | weight0[128,256] |
| 15-21 | window | 256 | 8 | 4,224 (88x48) | branched | |
| 22 | window+ds | 256→512 | 8 | 4,224 | branched | pad-to-8 then pool; weight0[256,512] |
| 23-30 | split-window | 512 | 16 | 1,056 (44x24) | split-group | bias swizzled (H=16) |
| 30.ds | plain ds | 512→1024 | — | 1,056 | — | pool(HALF-only) then layer4.weight |
| 31-38 | global | 1024 | 32 | 288 (24x12) | global | full attention, ±3 cap, scale·√32 |
| 39 | conv+merge | 1024→512 | — | 288→1,056 | — | GEMM, up2, +skip·sin, e4m3 |
| 40-47 | split-window | 512 | 16 | 1,056 | split-group | |
| 48 | up-transition | 512→256 | 8 | 1,056→4,224 | — | weight0[512,256] + sin |
| 49-55 | window | 256 | 8 | 4,224 | branched | |
| 56 | up-transition | 256→128 | 4 | 4,224→16,896 | — | weight0[256,128] + sin |
| 57-61 | window | 128 | 4 | 16,896 | branched | |
| 62 | up-transition | 128→64 | 2 | 16,896→67,584 | — | weight0[128,64] + sin |
| 63-65 | window | 64 | 2 | 67,584 | branched | |
| 66 | up-transition | 64→32 | 1 | 67,584→270,336 | — | weight0[64,32] + sin |
| 67-69 | window | 32 | 1 | 270,336 | plain | |
| 70 | window + head | 32 | 1 | 270,336→1,081,344 | plain | merge sin/cos, no publish, head GEMMs |

(Extent chain: 1408x768 →pool→ 704x384 → 352x192 → 176x96 → 88x48 → 44x24 →pad48x24→pool 24x12 → bottleneck → back up. All levels 8-aligned; window counts per level: b0 1408·768/64 = 16,896 windows; then 4,224 / 1,056 / 264 / 66 / 11·... at 44x24: (44/8+1)x(24/8+1) worst case shifted = 6x4=24 → with origin (0,0) 5x3=15; shifted (+1 row/col) 6x4=24. Worst-case per-level window counts govern scratch sizing, not average.)

## B. Weight map

All 649 tensors live in ONE device-local VkBuffer (M6a already uploads `work/mlxw/dlssnr-logical.safetensors` as a single 291,535,872 B buffer with a name→offset map). M6b keeps that buffer and adds **load-time preprocessing** (below); the repack is done once in the staging copy, so device offsets stay as computed here.

### B.1 Layout

- Order: block0→block70, within a block sorted by tensor name; each tensor 256 B-aligned. Computed from the real header (`docs/pack-layout.txt`, regenerable via `docs/pack_dump.py`): total **291,535,872 B = 278.03 MiB** (24 KB alignment slack over raw 278.01 MiB — negligible).
- Block order == graph access order, so this is simultaneously "access-order grouping". Per-block offset table (start/span, bytes):

| blocks | offset | span each | family |
|---|---|---|---|
| b0 | 0 | 34,560 | stem window32 + input_adapter |
| b1-3 | 34,560 | 33,536 | window32 |
| b4 | 135,168 | 37,632 | window32 + weight0[32,64] |
| b5-7 | 172,800 | 107,264 | window64 branched |
| b8 | 494,592 | 123,648 | window64 + weight0[64,128] |
| b9-13 | 618,240 | 361,216 | window128 branched |
| b14 | 2,424,320 | 426,752 | window128 + weight0[128,256] |
| b15-21 | 2,851,072 | 1,312,000 | window256 branched |
| b22 | 12,035,072 | 1,574,144 | window256 + weight0[256,512] |
| b23-29 | 13,609,216 | 3,803,392 | split512 |
| b30 | 40,232,960 | 4,851,968 | split512 + layer4.weight[512,1024] |
| **b31-38** | **45,084,928** | **25,170,432 each** | **global1024 — HOT REGION** |
| b39 | 246,448,384 | 1,049,600 | conv merge |
| b40-47 | 247,497,984 | 3,803,392 | split512 |
| b48 | 277,925,120 | 1,574,656 | window256 + weight0[512,256] + sin |
| b49-55 | 279,499,776 | 1,312,000 | window256 |
| b56 | 288,683,776 | 427,008 | window128 + weight0[256,128] + sin |
| b57-61 | 289,110,784 | 361,216 | window128 |
| b62 | 290,916,864 | 123,904 | window64 + weight0[128,64] + sin |
| b63-65 | 291,040,768 | 107,264 | window64 |
| b66 | 291,362,560 | 37,888 | window32 + weight0[64,32] + sin |
| b67-69 | 291,400,448 | 33,536 | window32 |
| b70 | 291,501,056 | 34,816 | window32 + head tensors |

- **Hot region: bytes [45,084,928, 246,448,384) = 192.04 MiB (blocks 31-38).** 66% of all weight bytes stream through the 8 global blocks every frame. Everything else is "cold" by comparison. On B50 (16 GB device-local, heap 16,206 MB reported in M6a) the whole buffer is 1.7% of VRAM — residency is trivial; "hot" matters only as a profiling hint and as the region to keep tightly packed for L2/LLC friendliness (it already is contiguous).

### B.2 Tensor roles per family (name → op → shape; dtype F16 unless noted)

**window32 (b0-4, 62-70 core):** `input_adapter_weight [16,32]` stem GEMM (b0 only) · `qkv_weight [32,96]` attn QKV · `projection_weight [32,32]` attn out-proj · `attn_bias [1,64,64]` window bias (H=1: **fragment-swizzled**) · `attn_scale [1]` F32 per-head Q scale · `attn_cos_skip [32]` closing residual gate · `weight1 [32,128]` FFN expand · `weight2 [128,32]` FFN contract · `ffn_cos_skip [32]` FFN residual gate. Downsample variants add `weight0 [32,64]/[64,128]/[128,256]/[256,512]` (b4/8/14/22) or `[512,256]/[256,128]/[128,64]/[64,32]` (b48/56/62/66) + `sin [C]` skip scale. b70 adds `out_gain [16,4]`, `out_conv_weight [16,4]` head GEMMs, `inp_merge_sin/cos [32]`, and `blend_scale [1]` (**unused by the graph** — present in file, do not bind).

**window64/128/256 branched (b5-22, 48-61, 63-65):** `qkv_weight [C,3C]` · `projection_weight [C,C]` · `attn_bias [H,64,64]` (H=2/4/8: stored logical) · `attn_scale [H]` F32 · `attn_cos_skip [C]` · `ffn_cos_skip [C]` · `ffn_expand_weight [H,4,H,32,32]` · `ffn_branch_projection_weight [H,4,32,32]` · `ffn_output_projection_weight [C,C]`.

**split512 (b23-30, 40-47):** `layer0.first_projection_weight [512,512]` · `layer0.group_expand_weight [8,64,256]` · `layer0.group_project_weight [8,256,64]` · `layer1.weight3 [512,512]` · `layer1.ffn_cos_skip [512]` · `layer2.qkv_weight [512,1536]` · `layer2.attn_bias [16,64,64]` (**fragment-swizzled**) · `layer2.attn_scale [16]` F32 · `layer3.projection_weight [512,512]` · `layer3.attn_cos_skip [512]`. b30 adds `layer4.weight [512,1024]`.

**global1024 (b31-38):** `layer0.weight [1024,4096]` FFN expand · `layer1.weight [4096,1024]` FFN contract · `layer1.ffn_cos_skip [1024]` · `layer2.qkv_weight [1024,3072]` · `layer2.attn_scale [32]` F32 · `layer3.attention_scalar [1]` F16 · `layer4.projection_weight [1024,1024]` · `layer4.attn_cos_skip [1024]`.

**b39:** `layer0.conv_weight [1024,512]` projection GEMM · `layer0.inp_upsample_sin [512]`.

### B.3 Load-time preprocessing (host, during the staging memcpy — zero extra GPU passes)

1. **Bias unswizzle** for `attn_bias` of H∈{1,16} blocks (b0-4, b23-30, b40-47, b62-70… i.e. every block whose family table marks it): apply `FRAGMENT_SWIZZLE_INDICES` (`nr_model._fragment_swizzle_indices`) per head while copying to staging. Output layout logical `[H,64,64]`.
2. **Branched-FFN fuse-fold**: for each branched block pre-fold `ffn_expand_weight → per-head expansion (C,128)` (`transpose(0,2,3,1,4).reshape(H, H*32, 128)`) and `ffn_branch_projection_weight → (H,128,32)`. Relayout only — same bytes, same buffer offsets (offsets are per-tensor start; the folded tensor replaces the original in place; keep original layout instead if a validation stage needs it — decide at first golden compare, see §E).
3. **attn_scale side table**: small host-visible scratch with, per global block, `attn_scale` and `attn_scale·√32` in F32 (for the Q-publish); window blocks need only `attn_scale`. Also duplicate per-head into a device buffer for shader uniform reads if push-constant space is tight (H≤32 → ≤128 B per block — push constants suffice).
4. **F16 subnormal audit**: M6a stats showed weights are clean (min magnitudes ≥ ~2^-11 band), consistent with ARCHITECTURE.md §4 (E4M3 decode can't produce FP16 subnormals). Activations are the subnormal risk — handled at runtime by the same `2^k` rescale rule if ever observed in practice (see §F; likely unnecessary).
5. `blend_scale` (b70) is loaded but never bound.

### B.4 What we deliberately do NOT do

- No E4M3 re-encoding of weights (logical file is already decoded F16 — the vendor storage format is irrelevant to us).
- No int8 path (`notes/phase23-integer-weights.md` is the reference's closed investigation — measured, not adopted).
- No weight compression/reordering beyond B.3 — GEMM K-dims are already multiples of 16 (32/64/128/256/512/1024, 96/192/384/768/1536/3072 for qkv-out, 4096) so the 8x16x16 coopmat config needs no K-padding except `input_adapter` K=16 (exact) and head K=16 (exact). N-dims: 4 (head out) < 16 → pad head GEMM N to 16 with zeros at fold time (only out_gain/out_conv; 128 B each).

## C. Shader plan

GLSL → `glslangValidator --target-env vulkan1.3` (same toolchain as M0/M3). **Reuse M3's verbatim `shaders/publish.glsl`** (already audited, byte-copied from `src/gpu/publish.glsl`) for every rounding point. All shaders get matrix/vector operands as **64-bit device addresses via `GL_EXT_buffer_reference` push constants** — no per-pass descriptor sets, exactly like `gemm_resident.comp` (`src/gpu/`, analysis §6.1). One empty descriptor set layout bound at pipeline layout creation to satisfy Vulkan.

### C.1 GEMM kernel (the workhorse)

- Coopmat config: **8×16×16, f16×f16→f32, subgroup scope** (M0-probed config [0] on the B50; `TM,TN,TK = 8,16,16`).
- **Matrix contract (K-by-N, from M6a metadata):** `out[M,N] = in[M,K] @ w[K,N]`; both `in` and `w` row-major. A-tile loaded row-major from activations; B-tile loaded **column-major** from the weight — a col-major coopmat load of a row-major `K×N` buffer yields `Bᵀ` per the vendor layout, which is what `coopMatMulAdd` consumes; the reference exploits the identical trick (`xmx.py:matmul_nt`, analysis §6.3). For `Q@Kᵀ` (scores) the same kernel takes `transpose_b` + batch/head strides — zero transpose copies anywhere.
- Register blocking RM=2, RN=2 → 16×32 tile per subgroup; K-loop steps 16 with FP32 accumulator (deliberate: reference `notes/phase4` — FP32 accum is 400–800× more accurate than the vendor's FP16 accum and is the port's stated choice; ARCHITECTURE.md §5).
- Grid: `ceil(M/16) × ceil(N/32)` workgroups, `local_size = 32` (one subgroup per workgroup — simplest barrier-free tile; revisit with 2-subgroup workgroups only if profile says launch-bound).
- **Fused epilogues** (push-constant `flags`, bits 8-11 — mirror `xmxres.EPI_*`): NONE | HALF (`half_round`) | E4M3 | GATE | GATE_E4M3. Applied on the FP32 accumulator in FP32, single conversion at the end (except GATE's internal half-round chain, §A.1).
- **A-input modes:** A_F32 (read fp32, `half_round` in-kernel — this replaces the reference's separate `to_half` pass) or A_F16 (direct load; used after any e4m3/half publish, which is exact in f16).
- Special forms via the same source + defines/spec-constants: `batch` + per-operand byte strides (attention per-head GEMMs), optional `attn_bias` add on the accumulator (fp32, pre-softmax; window scores only), `N-pad` (head GEMMs, N=4→16 zero-padded at fold time, §B.3.4).

### C.2 Elementwise / data-movement shaders (one file, `kind` dispatch like `resident.comp`)

| shader | function | rounding |
|---|---|---|
| `partition.comp` | NHWC fp32/f16 → window order `(nw,64,C)` f16, zero-pad for shifted origin, fuse `half_round` when reading fp32 | HALF on output |
| `gather_residual.comp` | closing residual: `out = attn_branch_win + ffn_out_img * attn_cos` with **reverse-window gather** fused on `attn_branch` read; optional E4M3 epilogue (block publish) | E4M3 on output |
| `residual.comp` | plain `out = branch + skip * cos_vec` (FFN internal residual; b39/b48/56/62/66 merges) | optional E4M3 |
| `cosine_publish.comp` | fragment-tree normalize (§A.5 exact lane order, FP16 arithmetic with saturating overflow) + optional per-head scale + E4M3; `qkv_part` select (Q/K/V from packed proj) | E4M3 |
| `softmax.comp` | bit-affine softmax (§A.6 exact chain, u32 bit trick, fp32 row-sum rounded once, f16 reciprocal) over rows of 64; window variant fuses `attn_bias` add; global variant fuses symmetric ±3.0 cap and even-row padding | E4M3 |
| `pool2.comp` | 2×2 mean (fp32 ×0.25) → optional pad-to-multiple first; E4M3 (window ds) or HALF (block 30) | E4M3 / HALF |
| `upsample2.comp` | nearest 2× repeat + crop to skip extent | — (fp32) |
| `scale_channel.comp` | `out = a * sin_vec` (skip path) | — |
| `add3.comp` | `out = up + scaled_skip` (+E4M3 for transitions) | E4M3 |
| `head_merge.comp` | b70 pre-merge `up*sin + skip*cos` (fp32, no publish) + `tokens[…,:16]@out_gain + tokens[…,16:]@out_conv` head GEMMs (via gemm kernel, §C.1) | none |
| `to_half.comp` | fp32→f16 bulk convert (only where no GEMM consumes the fp32 directly) | HALF |

### C.3 Dispatch graph per block (passes in record order; barrier = `vkCmdPipelineBarrier` storage-buffer W→R between dependent passes)

**window block (b1-3, 5-7, 9-13, 15-21, 49-55, 57-61, 63-65, 67-69):**
```
1  gemm   x16 = half(x)                         [A_F32, fused HALF]        (skip if x already f16)
2  gemm   h16 = GATE_E4M3(x16 @ w_expand)        branched: G × [per-head (C,128)] GEMMs, EPI_GATE_E4M3
   gemm   branch = e4m3(gate(x16@w1)) @ w2       plain C=32 path, EPI_GATE_E4M3 on first
3  gemm   heads16 = E4M3(h16 @ branch_proj)      branched only, G GEMMs (128→32)
4  gemm   branch = heads16 @ ffn_out_proj        (branched) — no epilogue
5  residual ffn = branch + x*ffn_cos             fp32
6  partition win16 = windows(ffn)                f16, origin-aware pad
7  gemm   proj = win16 @ qkv                     (A_F16)
8  cosine_publish q16 (scaled), k16;  split v16=E4M3   (3 dispatches, independent)
9  gemm   scores = q16 @ k16ᵀ  per head          batch=H, transpose_b
10 softmax probs16 = softmax(scores + bias)      window: bias fused, no cap
11 gemm   ctx = probs16 @ v16 per head           batch=H
12 cosine… no — merge_heads+E4M3  (fused into gemm A-load? No:)
12 gemm   attended = E4M3(merge(ctx)) @ proj_w   merge+E4M3 as small gather pass or fused A-path in gemm (recommend separate 20-line `merge_heads.comp` for clarity)
13 gather_residual out = E4M3(attended_win→img + ffn*attn_cos)
```
≈ 13-15 dispatches per window block (branched), 11 for plain.

**split block (b23-30, 40-47):** `gemm first(E4M3)` → 8× (`gemm expand GATE` + `gemm project E4M3`) → concat implicit via strided writes → `gemm weight3` → `residual` → attention 6-13 (H=16, bias swizzled at load). ≈ 24 dispatches.

**global block (b31-38):** `gemm expand[1024,4096] GATE_E4M3` → `gemm ffn_proj` → `residual` → `gemm qkv` → cosine_publish×2 + v-split → `gemm scores (288×288, batch 32, transpose_b)` → `softmax(cap=±3)` → `gemm ctx` → `merge_heads E4M3` → `gemm proj` → `residual(E4M3 publish)`. 13 dispatches. Scores fp32 buffer = 32·288·288·4 = 10.6 MB (global only — window scores stay 64×64 per window).

**transitions:** down: `pool2(E4M3)` → `gemm weight0(E4M3)`; b22 pads first; b30 pool-HALF + `gemm layer4.weight(E4M3)`. up: `gemm weight0` → `upsample2` ∥ `scale_channel(skip)` → `add3(E4M3)` → window block. b39: `gemm conv_weight` → `upsample2` ∥ `scale_channel` → `add3(E4M3)`. b70: `upsample2` ∥ … → `head_merge` → 2 head GEMMs.

Whole-graph record: ~1,300–1,500 dispatches, recorded ONCE per extent into a command buffer, replayed per frame (`xmx_graph_capture` analogue; reference measured replay bit-identical, `notes/phase27`).

### C.4 Workgroup sizing at 1408×768

GEMM grids per level (M=tokens): b0 M=1,081,344 (grid 67,584×3 for N=96 with 16×32 tiles → e.g. qkv N=96: 67,584 × 3 = 202k WG); levels 704×384 M=270,336; 352×192 M=67,584; 176×96 M=16,896; 88×48 M=4,224; 44×24 M=1,056; global M=288 (grid 18×96 for N=3072 = 1,728 WG, K-loop 64 iters — the latency-critical block; consider RM=1/RN=4 there if profile demands). Elementwise: one workgroup per 64×C tile row (e.g. 256 threads, 8 px/thread at C=32 full res). Softmax: one subgroup per (window,head) row → b0: 16,896 windows × H … window softmax rows = windows×H (b0 H=1: 16,896 subgroups).

### C.5 Accumulation & rounding contract (must match publish.glsl exactly)

1. GEMMs: FP32 accumulators end-to-end (K-loop and multi-tile reduction). Only conversions at epilogues.
2. `half_round` via `packHalf2x16`/`unpackHalf2x16` ONLY (compiler elides plain casts — `publish.glsl:10-16`).
3. `e4m3`/`gate_activation` verbatim from M3's `publish.glsl`; gate marked `precise`.
4. FP16 elementwise islands (cosine fragment tree, softmax weights) do arithmetic in fp16-carrying-fp32 with a half_round after EVERY op (half_add/half_multiply/half_fma semantics — fma rounds once).
5. Softmax row sums: fp32 accumulate, round once to f16; reciprocal f16; final weight product fp32→f16; then e4m3.
6. Everything else fp32: residuals, bias add, pool, transitions merges, head, composition.
7. Known acceptable divergence: branched-FFN fused fold reassociates fp32 sums by ~1e-7 (§A.7); GPU path may additionally skip the FFN-internal e4m3 that the Linux runtime fused into branched residuals (we follow the numpy spec instead — §A.8). Both are below the e4m3 quantum; validated in §E stage 2.

## D. Executor architecture

### D.1 Plug-in points (M3 pipeline, unchanged contracts)

```
DDA → decode.comp → letterbox → rescale.comp(down) → features.comp ──▶ [M6b executor] ──▶ compose.comp → rescale.comp(up) → encode
                                                            16ch fp32                                        4ch fp32
                                                          (1408×768×16)                                    (1408×768×4)
```
- **Input:** `feat_buf` — the exact buffer M3 `features.comp` already writes (16-ch fp32 @ 1408×768, 69.3 MB). The M3 stand-in block dispatch (`standin.comp`) is deleted; the M6b graph dispatches are recorded in its place. Stem GEMM reads `feat_buf` directly (A_F32 mode, §C.1) — no copy.
- **Output:** `head_buf` 1408×768×4 fp32 (17.3 MB) in the exact layout `compose.comp` expects (RGB residual + gate logit). Composition formula unchanged (§A.9).
- Same device (LUID 95a2 pick), same compute queue, same per-frame command buffer: M6b appends its dispatches between the features and compose sections of the buffer M3 already records per frame. No new semaphores/fences; barriers are plain `vkCmdPipelineBarrier` inside the buffer.
- v1 keeps M3's first-frame history convention (ch7-9 = ch4-6) and M3's stand-in noise for ch0-2 (flagged; real `deterministic_noise` port is a named follow-up, ~40 lines, host-precomputed per frame index into a 3-ch f16 buffer).
- `blend_scale`, per-pixel control mask, profiles beyond (0,1,1): not exercised by the desktop pipeline; hooks reserved in push-constant block.

### D.2 Components

1. **Weight residency** (from M6a, extended): safetensors parse → single device-local VkBuffer 291,535,872 B; staging memcpy performs §B.3 preprocessing (bias unswizzle, branched fold, head N-pad, attn_scale side table). Cold-start ≈ 0.6 s (M6a-measured).
2. **Scratch arena**: one bump-allocated device buffer per recorded extent (reference `ScratchArena` pattern; their 720p arena: 5,041 → 2,303 MiB after pooling — `notes/phase32`). Allocation is static at record time; addresses baked into push constants, so replay needs no re-allocation.
3. **Recorder**: `record(extent)` walks the 71-block dispatch table (§A.10), emitting ~1,300–1,500 dispatches + minimal barriers into a dedicated secondary-command-buffer-per-extent (or the primary, matching M3's current style). Recorded once per extent; per frame = one `vkQueueSubmit` of the recorded chain (reference: replay bit-identical, `notes/phase27`).
4. **Barrier minimization (v1.5, not v1)**: v1 barriers between every dependent pass (correctness first). Safe elisions later: WAW on the same buffer with no interleaved read can drop to an execution barrier; the 8 independent group GEMMs of split-FFN already record without inter-barriers (disjoint slices, the reference `independent()` pattern).
5. **Extent handling**: record keyed by (H, W); 1408×768 (render_scale 0.55 of 2560×1440) is the only extent exercised; the letterbox/align code must still pad+crop for other extents per §A.2 contract (extent ≥320, multiple of 64; mirror-pad).

### D.3 Activation scratch estimate @ 1408×768 (fp32 carriers, f16 published activations)

Per level: published value ping-pong f16 `2·T·C·2 B`; FFN residual fp32 `T·C·4`; qkv proj fp32 (windowed) `W·64·3C·4`; scores fp32 `W·H·64·64·4`; probs f16 half of scores; ctx/merged f16 `W·64·C·2`.

| level | T (tokens) | C | approx. scratch |
|---|---|---|---|
| b0 (1408×768) | 1,081,344 | 32 | 1.55 GB (proj 415 MB, scores 277 MB, carriers 138 MB, ffn 138 MB, q/k/v 208 MB) |
| b1-4 (704×384) | 270,336 | 32→64 | 0.42 GB |
| b5-8 (352×192) | 67,584 | 64→128 | 0.28 GB |
| b9-14 (176×96) | 16,896 | 128→256 | 0.20 GB |
| b15-22 (88×48) | 4,224 | 256→512 | 0.19 GB |
| b23-30 (44×24) | 1,056 | 512 | 0.21 GB |
| b31-38 (24×12) | 288 | 1024 | 0.03 GB (scores 10.6 MB fp32) |
| b39-47 (44×24) | 1,056 | 512 | 0.21 GB |
| b48-55 (88×48) | 4,224 | 256 | 0.19 GB |
| b56-61 (176×96) | 16,896 | 128 | 0.20 GB |
| b62-65 (352×192) | 67,584 | 64 | 0.28 GB |
| b66-70 (704→1408×384→768) | 270,336–1,081,344 | 32 | 0.45 GB (b70 runs at full res after merge) |

**Total ≈ 4.1 GB** worst case with per-level arenas and no sharing; ~2.6 GB if levels share arenas on the (stack-like) U-Net liveness profile. + 0.28 GB weights + 0.09 GB feature/head = **~3 GB device footprint** — 18% of the B50's 16.2 GB heap. No swapping, no staging at steady state. (Reference at 720p: 2.3 GiB — consistent.)

### D.4 Frame loop

```
per frame:
  DDA acquire → import → timeline wait (existing M3 prologue)
  cmd = begin recording
    features section (existing)                    → feat_buf
    graph section:  vkCmdExecuteCommands(graph_cb[extent])   [recorded once]
    compose section (existing)                     → head_buf → final image
  end; submit; present (existing M3 epilogue)
```
`graph_cb[extent]` is only re-recorded when extent or weight buffer changes. First-frame and steady-state paths identical.

## E. Validation plan

Golden source, in order of authority:
1. **`reference/dlss-nr-on-intel/src/ref/nr_model.py` — pure NumPy, runnable TODAY (no torch needed).** Hookable `MATMUL`/`MATMUL_NT`, per-block `progress` callback, exact vendor rounding chain. This is the primary golden.
2. `work/mlx-dlss/python/mlxdlss/model.py:844` `NeuralRenderingModel` (torch) — independent cross-check. **Do NOT install torch now.** Later: `python -c "from mlxdlss.model import load_model; m = load_model(r'work/mlxw/dlssnr-logical.safetensors'); ..."` (entry `load_model` at model.py:1094 opens the safetensors directly; `forward(input_value)` at :1005 takes `[1,H,W,16]` fp32). First verify numpy-ref ≈ torch on the same input (validates golden #1), then use torch dumps if numpy and Vulkan disagree.

All stages use a hand-crafted 16-ch input (deterministic per-channel patterns: ramps, constants, ±0.5 blocks) at **320×320** (extent floor; exercises pool/pad/window paths; CPU reference runs in seconds).

### Stage 1 — stem-only vs hand-computed input
Adapter GEMM + block0 only. Thresholds:
- Adapter output (fp32): max rel err vs numpy < 1e-5.
- Block0 published output (f16 e4m3 grid): **≥ 99.99% exact f16-bit match**; remainder within 1 e4m3 step. (Elementwise chain through fixed publishes should be near-bitwise; GEMM order is the only wobble.)

### Stage 2 — single block vs reference golden
Driver: import `nr_model`, `load_logical(...)` on our safetensors, monkey-patch `window_block`/`branched_window_block`/`split_window_block`/`global_block`/`record_*`-equivalents to dump `{block_in, ffn_out, attn_branch, block_out}` per invocation to `.npy`. Blocks to dump (one per family): **b0 (stem/plain32), b5 (branched64), b23 (split512), b31 (global1024), b48 (up-transition), b70 (head)**. Thresholds:
- At every publish boundary (block_out): ≥ 99.9% exact f16-bit match vs numpy, remainder ≤ 1 e4m3 step.
- Internal fp32 tensors (block_in, ffn_out): cosine similarity > 0.999999, max rel err < 5e-4 (below e4m3 quantum; chaos has not yet amplified — `notes/phase9`).
- Specifically exercise: shifted origins (b5's origin ≠ (0,0)), bias swizzle (b23, H=16), global cap (b31), the b70 two-GEMM head split at channel 16.

### Stage 3 — full-chain smoke @ 1408×768 (real M3 snapshot input)
- No NaN/Inf anywhere in head_buf; `head.rgb`: |mean| < 0.02, std in [0.005, 0.1] (calibrate from stage 2); gate logit mean ∈ [-6, +6].
- e4m3 publish histogram: fraction clamped at ±448 < 1e-4 (health indicator).
- 320×320 numerical full-chain: block70 output cosine sim > 0.9999 vs numpy; composed image **PSNR > 35 dB** vs numpy-composed (image-level metric — per-pixel equality is impossible by chaos, `ARCHITECTURE.md` §5).
- Output stats vs M3 stand-in (2560×1440 metrics): mean|final−native| > 0, spatial structure mean|grad(δ)| > 6.7 (M3's value — must exceed, not merely match).

### Stage 4 — quality vs M3 stand-in
30-frame idle-desktop sequence through the M4-SIMPLE loop: temporal flicker (mean |frame-to-frame delta| over static regions) ≤ M3's; structure metric ≥ M3's; visual BMP inspection clean (no mottling/mask artifacts — the reference's `notes/phase43` failure mode). Pass bar: no metric regresses, structure improves.
Perf gate: per-pass GPU timestamps on global blocks (reference `notes/phase45` method): f16 GEMM ≥ 3 TFLOP/s sustained (Arc 140V measured 3.83); softmax+cosine passes ≤ 20% of total frame GPU time.

## F. Risks + unknowns

### F.1 Performance risk (the big one)

Rough FLOP estimate @ 1408×768 (2·MACs·tokens, attention included): b0 ≈ 35 GF; b1-3 ≈ 9 GF each; b4 ≈ 9.5 GF; b5-7 ≈ 7 GF each; b8 ≈ 8 GF; b9-13 ≈ 6 GF each; b14 ≈ 6.5 GF; b15-21 ≈ 5.5 GF each; b22 ≈ 5.7 GF; b23-30 ≈ 4 GF each; **b31-38 ≈ 7.5 GF each (60 GF total)**; b39 ≈ 1.2 GF; b40-47 ≈ 4 GF each; b48-55 ≈ 5.5 GF each; b56-61 ≈ 6 GF each; b62-65 ≈ 7 GF each; b66-69 ≈ 9 GF each; b70 ≈ 9 GF. **Total ≈ 460 GFLOP/frame.**

Budget: 30 fps → 33 ms → needs ~14 TFLOP/s effective; 60 fps → 16.7 ms → ~28 TFLOP/s. Evidence: Arc 140V (Xe2, Mesa) sustains 3.83 TFLOP/s coopmat (`notes/phase33`); B50 is a larger Xe2 part but M0 only proved correctness, not rate. **Honest projection: 30–90 ms/frame → 11–30 fps for the full graph at 1408×768** until fusion/barrier work lands (reference: 412 ms @1080p-class on 140V with everything already tried — `notes/phase45/46`). The 37.6 fps M4-SIMPLE loop will drop. Mitigations: render_scale 0.4 (extent 1024×576 → ~55% of the FLOPs), barrier elision, fused epilogues (already in plan), persistent-style GEMM with L2 tiling for the hot region.

### F.2 Explicit open questions
1. **B50 sustained coopmat rate** — unknown; measure with the stage-2 b31 prototype before building all 71 blocks (this is why the prototype order below is what it is).
2. Branched-FFN fused fold (§B.3.2): keep both layouts bindable; decide after first stage-2 compare.
3. Window-attention scratch counts must use each level's worst-case origin (-4,-4) window count (reference `BlockScratch` comment: silent corruption if undersized) — our per-level arenas (§D.3) already are worst-case; confirm at record time with an assert.
4. Does the B50 driver prefer one-subgroup-per-workgroup GEMMs or multi-subgroup tiles? Profile-driven; design allows both (spec constant).
5. Noise channels ch0-2: real `deterministic_noise` port timing (M6b runs with M3's stand-in hash — output difference vs golden must be attributed correctly in stage 3).
6. Temporal path (history ch7-9, gate blending across frames) deliberately deferred — full benefit needs M7; v1 is per-frame with first-frame convention.

### F.3 Lower risks
- XMX f16 subnormal flush (`notes/phase4`): weights clean (M6a stats), e4m3 publishes can't produce f16 subnormals; only the in-GEMM `half_round` of tiny fp32 carriers could — apply the reference `2^k` rescale (`xmx.py:_shift`) if a golden mismatch correlates with subnormal operands.
- Softmax u32 bit-trick in GLSL: needs `GL_EXT_shader_explicit_arithmetic_types{,_int*`; bit ops are exact — low risk but unit-test against a CPU table (stage 1 includes softmax via block0).
- Chaos-driven validation flakiness: thresholds in §E are distribution-based for this reason.
- Weight licensing: NVIDIA proprietary, local research only, file already outside git (`work/`), never commit.

### F.4 Recommended first prototype (in order)
1. **M0-extension unit kernel**: one-tile GEMM with all 5 epilogues + cosine_publish + softmax vs CPU bit-exact tables (validates every rounding point in isolation, hours of work).
2. **Global block 31 end-to-end** @ 24×12 vs nr_model golden (§E stage-2 harness): simplest family (no windows/partition), the hot 192 MiB region, and it directly measures achieved TFLOP/s → go/no-go for the full build.
3. **Window block 5** (branched FFN + shifted windows + full attention numerics) @ 352×192 in-graph.
4. Full 71-block chain (this design), then stages 3-4.

### F.5 Blockers
None known for implementation. (Torch install for the optional cross-check is deliberately deferred, not blocking — numpy golden suffices.)
