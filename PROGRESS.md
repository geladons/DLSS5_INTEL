# DLSS5_INTEL — Progress Log

Project: Intel Arc port of DLSS 5-style neural rendering, whole-desktop
(NeuralScreen analog). Root: `C:\Users\AI\Desktop\DLSS5_INTEL`

## Done (2026-09-19)

- [x] Workspace: `reference/` (5 cloned repos, read-only) + `dlss5/` + README.md
- [x] git repo initialized (initial commit `26494de`; `reference/` gitignored)
- [x] Dev environment inventory: **Arc Pro B500 (Battlemage) + B50**, 32 GB,
      Win11 Pro WS, Git 2.55, Node 24, .NET, VS Build Tools 2019;
      missing Python/VulkanSDK/CMake/Rust
- [x] `win-desktop-control` skill (local, vetted) — screenshots, windows,
      UIA read/elements, clicks, keys (with documented host quirks)

## In progress

- [ ] Deep analysis of `reference/dlss-nr-on-intel` (Linux Vulkan layer →
      Windows port path, XMX/cooperative_matrix requirements)
- [ ] Deep analysis of `reference/NeuralScreen` (capture → inference →
      present pipeline, overlay architecture, app/window modes)

## Toolchain (2026-09-19)

- Vulkan SDK 1.4.357.0 (vulkaninfoSDK.exe at C:\VulkanSDK\1.4.357.0\Bin\)
- CMake 4.4.3 (C:\Program Files\CMake\bin) — not on stale PATH of running processes, use full path
- Python 3.12.10 (%LOCALAPPDATA%\Programs\Python\Python312) — same PATH caveat
- VS Build Tools 2019 with VC.Tools.x86.x64 (cl.exe available)
- ninja: not found (use VS generator / MSBuild; non-blocking)
- **GPU reality check:** the PCI GPU is **Intel Arc Pro B50** (VEN_8086&DEV_E212,
  driver 101.8805); the "Arc Pro B500" entry is a root-enumerated virtual display
  (ROOT\DISPLAY\0000, driver 11.30.4.434) — not a real Vulkan device.
- **VK_KHR_cooperative_matrix: SUPPORTED (rev 2) on the B50**, compute stage only,
  apiVersion 1.4.348. Full report: docs\vulkaninfo-gpu.txt — M0 gate PASSED.
- UAC: ConsentPromptBehaviorAdmin=0 (elevate w/o prompts), SmartScreen off (owner-approved fix).

## M4-SIMPLE result (2026-09-19) — PASS

Live processed-desktop overlay works. Built from M3 baseline (CPU bridge,
zero-copy explicitly DEFERRED). Run: 300 frames in 7.98 s — **avg 37.6 fps,
rolling 39.0**, dropped 0, present FIFO via blit.comp; verify frames
10/30/60 ALL PASS (mean|final−native| and structure > 0; deltas shrink on an
idling desktop — expected residual physics with stand-in transform).
Artifacts: dlss5/m4-present-simple/, docs/m4-simple.log.

**Deferred to M5 (own design doc):** zero-copy live loop (m4-live-present/,
3 timed-out attempts). Observed failure: per-frame reuse of ONE imported
shared texture + timeline semaphore fails the wait ~frame 4
("[FAIL] timeline wait frame N"); driver likely releases the shared texture
lazily. M2/M3 single-shot import is fine; M3 canary re-passed at 19:37
(driver healthy). M5 must use rotating N>=3 shared textures with per-slot
completion tracking (skip-frame instead of stall).

## M5 result (2026-09-20, final) — BACKLOGGED, WIP committed (624ef99)

Zero-copy live loop: 5 subagent runs + manual debugging across ~3.5 h.
Progress: 3 rotating shared textures import cleanly (dedicated allocs),
device-ext fix applied (ext_mem_win32 + ext_sem_win32 now enabled). REMAINING
BLOCKER: the live loop deadlocks on the FIRST frame handoff — after DDA
content-frame acquire, before the first slot-import print; process goes idle
(CPU stalls, no output). The identical interop one-shot (M2/M3) is bit-exact
fine, so this is a cyclic-use deadlock (driver lazy release + fence epochs),
NOT a setup error. Next attempt must be a dedicated debug session with
VK_LAYER_KHRONOS_validation + apitrace, not blind reruns. Working live demo
remains M4-SIMPLE CPU bridge (37.6 fps). Pivoted to M6 (real graph) where
zero-copy is not the bottleneck.

## Weights (2026-09-20) — ACQUIRED

`work/mlxw/dlssnr-logical.safetensors` — 649 tensors, 291.5 MB (F16/F32),
71 transformer blocks, format `dlssnr-logical-v18` (what the reference
pipeline consumes). Source: nvngx_dlssnr.dll 310.8.0.0, SHA-256
`ceb6432f…62650` (the single build pinned by MLX-DLSS; hash-verified),
extracted with MLX-DLSS @ 06a3e11 (vetted: pure parsing, no network).
Full chain of custody: docs/weights-provenance.md; tensor inventory:
docs/weights-inventory.txt. Legal: NVIDIA proprietary — local research
use, NOT redistributable, not in git. Next: M6 — safetensors loader in the
Vulkan runtime + wire the real graph in place of the M3 stand-in block.

## M6b design (2026-09-20)

- Design doc (DESIGN-ONLY): `docs/m6b-graph-design.md` — Vulkan compute executor for the
  full 71-block DLSSNR graph, replacing the M3 stand-in block between features.comp and
  compose.comp. Sections A-F complete: op inventory with exact publish.glsl rounding
  contract, 278.03 MiB packed weight map (hot region = blocks 31-38, 192.04 MiB), shader/
  dispatch plan (8x16x16 f16 coopmat, K-by-N contract, buffer-reference push constants),
  executor architecture (~3 GB footprint @1408x768), 4-stage validation vs the NumPy
  reference golden (no torch needed), risks.
- Corrections established: attention is full MHA head_dim 32 (NOT GQA); branched FFN is
  dense (NO MoE router); packed-offset tables generated from the real safetensors header
  (`docs/pack-layout.txt`, regenerable via `docs/pack_dump.py`).
- Perf projection: ~460 GFLOP/frame @1408x768 → est. 30-90 ms/frame (11-30 fps) until
  optimized; first prototype = global block 31 (measures B50 coopmat rate, go/no-go).
- Blockers: none. Torch cross-check deferred (optional). Next: M6b implementation per doc.

## Next

- [ ] Toolchain install (Vulkan SDK, CMake, Python; VS Build Tools C++ workload check)
- [ ] Decide project stack (language, capture API: DXGI Desktop Duplication vs
      Vulkan layer; inference: DirectML / Vulkan compute / OpenVINO?)
- [ ] Milestone 1: capture a frame from a window on Arc + run any NN pass on it
- [ ] Vulkan on Arc B500: verify `VK_KHR_cooperative_matrix` support
      (`vulkaninfo` / capsule)

## Notes

- dlss-nr-on-intel targets Linux; port focus = Windows first, keep the Vulkan
  layer design portable (layers work on Windows too).
- Never run binaries/scripts from `reference/` without code review
  (ClickFix incident 2026-09-19, see workspace memory).
- Session work style: short steps, progress written here, long analysis
  delegated to sub-agents.
## M0 result (2026-09-19) - cooperative-matrix (XMX) probe: PASS

- Built dlss5/m0-coopmat-probe/ (raw Vulkan C++17, no libs beyond vulkan-1):
  main.cpp + probe.comp (GL_KHR_cooperative_matrix) + CMakeLists.txt + build.cmd.
- Device: Intel Arc Pro B50, apiVersion 1.4.348, driver 101.8805, subgroupSize 32.
- Enumerated 4 configs at runtime via vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR:
  [0] 8x16x16 f16xf16->f32, [1] 8x16x32 u8xu8->u32, [2] 8x16x32 s8xs8->s32,
  [3] 8x16x16 f16xf16->f16 - all scope=SUBGROUP. Chosen: [0] (f16->f32).
- Numeric verify: 8x16 tile, C = A*B (GPU coopMatMulAdd, f32 result) vs CPU f32
  reference: 128/128 within 1% tolerance (max rel err 0.0000%). XMX works.
- Timing: single dispatch+sync 0.473 ms; 100-dispatch batch avg 0.0033 ms/dispatch.
- Full build+run console log: docs/m0-coopmat-probe.log.
- Blockers: none. Notes: build orchestration is build.cmd (cmd batch) because the
  exec wrapper mangles $vars even in .ps1 -File mode; batch %VAR% is immune.
  Shader shape is injected via glslangValidator -D defines matching the runtime
  chosen config (probe writes chosen.txt beside probe.exe and exits 2 on mismatch
  so build.cmd recompiles + retries, max 3 attempts).

## M1 result (2026-09-19) - DXGI Desktop Duplication capture: PASS

- Built dlss5/m1-frame-capture/ (raw DXGI 1.2/1.5 + D3D11, C++17, deps: Windows
  SDK only — dxgi.lib/d3d11.lib): main.cpp + CMakeLists.txt + build.cmd.
- Device: D3D11 HARDWARE on Intel Arc Pro B50, feature level 11.1, VRAM 16.2 GB.
  Output \\.\DISPLAY5, 2560x1440, rotation IDENTITY, desktop format B8G8R8A8_UNORM.
- Duplication: IDXGIOutput5::DuplicateOutput1 pinning B8G8R8A8 was REJECTED by the
  driver (fell back to DuplicateOutput, driver still hands B8G8R8A8 — format-flap
  pinned path to re-test in M3). Errors for locked session / busy duplicator /
  disconnected session are mapped to clear messages (untested paths, no blocker).
- Capture loop: AcquireNextFrame(500ms) -> CopySubresourceRegion to staging ->
  Map -> BGRA8 CPU buffer; DXGI_ERROR_ACCESS_LOST handled with duplication
  re-create. Run: 300 attempts, 300 acquired, 0 timeouts, 37 dirty-updates /
  263 same-texture repeats, 0 errors.
- fps: avg 69.0 (interval min 3.98 ms = 251 fps, max 85.5 ms = 11.7 fps).
- 5 snapshots out\snapshot_{60..300}.bmp (14745654 B each, 2560x1440x4+54).
  Luma variance ≈ 9745, range [0,255] over 57.6k samples — REAL desktop content,
  not a black screen; snapshot_60 != snapshot_300 (cursor moved between grabs).
- Full build+run console log: docs/m1-capture.log (gitignored, force-added).
- Blockers: none. LESSON: DDA is dirty-rect driven — an idle VM desktop yields
  zero frames (first run: 300/300 timeouts). m1dda has a net-zero cursor-wiggle
  activity generator (argv[5]=0 disables) so the compositor produces frames.
## M2 result (2026-09-19) - D3D11(DDA) to Vulkan external-memory interop: PASS
- Built dlss5/m2-dxgi-vulkan-passthrough/ (C++17, DXGI/D3D11 + Vulkan SDK):
  main.cpp + passthrough.comp (invert) + CMakeLists.txt + build.cmd.
- Pipeline: DDA (DuplicateOutput, DISPLAY5 2560x1440 B8G8R8A8) -> CopyResource into
  D3D11_RESOURCE_MISC_SHARED|SHARED_NTHANDLE DEFAULT texture (+ staging twin) ->
  NT handle -> VkImage (OPTIMAL, STORAGE|TRANSFER_SRC) via
  VkExternalMemoryImageCreateInfo + VkImportMemoryWin32HandleInfoKHR on a DEDICATED
  allocation (driver reports requiresDedicated=1, prefersDedicated=1, 15,728,640 B)
  -> invert compute (rgba8 storage image) -> vkCmdCopyImageToBuffer -> BMPs.
- THE cross-API gate: VkPhysicalDeviceIDProperties.deviceLUID == DXGI adapter LUID
  00000000:000095a2 on "Intel(R) Arc(TM) Pro B50 Graphics". NOTE: Vulkan enumerates
  TWO physical devices with that same name (LUIDs 95a2 and 116cc) - LUID match is
  what picks the right one; do not pick by name/index in M3.
- Capability queries: D3D11_IMAGE->VkImage importable=1; D3D11_FENCE->semaphore
  importable=1. Sync = D3D11.5 fence (ID3D11Device5::CreateFence, SHARED NT handle)
  imported as Vulkan TIMELINE semaphore (vkImportSemaphoreWin32HandleKHR),
  Signal(1) after CopyResource + Flush, vkWaitSemaphores(1) before dispatch —
  GPU-side ordering, no host polling. Fallback (host GetCompletedValue poll) coded
  but not needed.
- Layout doctrine (vulkan-samples/Sascha Willems): image created initialLayout
  UNDEFINED, first op barrier UNDEFINED->GENERAL srcAccess=0. Validated by pixels.
- Verify: out\m2_inverted.bmp vs CPU inversion of out\m2_original.bmp (same frame):
  B/G/R = 100.0000/100.0000/100.0000% over 3,686,400 px each; alpha preserved
  100.0000% too. Exact equality -> interop proven bit-exact for this path.
- Timing run 2 (steady): acquire->D3D11 ready 22.6 ms; import 102.3 ms (includes
  one-time VkDevice creation); dispatch+copyback 12.5 ms; verify 233 ms (CPU loop);
  total 513 ms. Run-to-run: PASS twice (first had accumulated=0, second =1).
- Full build+run console log: docs/m2-interop.log (gitignored, force-added).
- Blockers hit & solved: (1) SDK 10.0.19041 d3d11.h does NOT chain-include
  d3d11_1..4.h - must #include <d3d11_4.h> explicitly for ID3D11Device5/Fence;
  (2) Vulkan 1.4 headers renamed the KHR handle type: use
  VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT (the old *_IMAGE_BIT name is
  gone); (3) win32 Vulkan types need VK_USE_PLATFORM_WIN32_KHR (CMake define).

## M3 result (2026-09-19) - neural pass slice on the reference pipeline: PASS

- Built dlss5/m3-neural-passthrough/ (C++17, D3D11/DXGI + Vulkan SDK): main.cpp
  (~62 KB) + 8 GLSL shaders + CMakeLists.txt + build.cmd. Copied from the M2
  skeleton; capture/import/sync doctrine unchanged (DuplicateOutput, DISPLAY5,
  NT-handle shared texture, dedicated alloc, D3D11.5-fence -> timeline semaphore,
  LUID 95a2 match — both "Arc Pro B50" Vulkan devices enumerated again, picked
  by LUID, not name).
- Pipeline ran end-to-end on a REAL captured desktop frame:
  decode -> letterbox scan -> rescale down -> 16ch features -> STAND-IN learned
  block -> rescale up -> residual compose -> encode -> readback -> BMPs.
- PORTED reference components (ports verified against in-tree reference sources):
  - nr_decode8      -> shaders/decode.comp   (ref src/ref/nr_image.c:36-44, bgra=1)
  - nr_encode8      -> shaders/encode.comp   (ref src/ref/nr_image.c:46-63; NaN->0,
                       unit clamp, +0.5 C-cast, alpha preserved; both full-frame and
                       region variants)
  - nr_compose      -> shaders/compose.comp  (ref src/ref/nr_image.c:53-76, blend=1,
                       subtract+add both kept for the FP32 rounding)
  - nr_features     -> shaders/features.comp (ref src/ref/nr_image.c:84-112; 16ch
                       layout incl. first-frame history=scaled colour, controls
                       ch10..14; ch0-2 noise = documented STAND-IN hash — the real
                       deterministic_noise() lives in vendored mlx-dlss features.py,
                       ABSENT from this clone)
  - resize/axis     -> shaders/rescale.comp  (ref src/layer/nr_daemon.py:238-255;
                       separable bilinear, weights subtract the CLIPPED low,
                       fp32 intermediate before 2nd axis)
  - active_region   -> host port in main.cpp  (ref src/layer/nr_daemon.py:261-296;
                       tol 2/255, 45% cap, +-1 symmetry, area>=half; GPU row/col
                       max reduction added, decision logic 1:1)
  - publish.glsl    -> shaders/publish.glsl  VERBATIM (ref src/gpu/publish.glsl:
                       half_round packHalf2x16 trick, e4m3, gate_activation —
                       the vendor rounding contract, ARCHITECTURE.md sec.4-5);
                       #included by features/standin/compose (not an entry point)
- STAND-IN components (loudly labelled): ch0-2 deterministic noise (see above)
  and the learned block itself (shaders/standin.comp: weight-free deterministic
  Laplacian of scaled colour gated by the verbatim vendor gate_activation through
  the e4m3/half publish chain, emitting the real 4-ch head layout). No weight
  files exist in the reference repo (dlssnr-logical.safetensors must be extracted
  from the user's own nvngx_dlssnr.dll) -> real 71-block U-Net port is out of scope.
- Metrics (region = full frame 2560x1440, 8-bit, final vs native original):
  per-channel mean|final-native| B=5.903 G=5.790 R=5.937 (>0 required); max|delta|
  = 255 levels; delta std = 29.44; pixels changed 18.33%; spatial-structure metric
  mean |grad(delta)| per ch B=6.724 G=6.680 R=6.698 (>0 => NOT a uniform/global
  shift). Artifacts: out\m3_final.bmp + out\m3_nr_out.bmp (content-verified,
  sample mean 103.25, std 101.08) + out\m3_native.bmp (raw capture diagnostic).
- Timing (ms, per-stage serialized submit+wait): acquire->D3D11 41.8; import+sync
  160.5 (one-time VkDevice); decode 0.79; letterbox 0.80; rescale down 171.3
  (first-touch page faults on fresh multi-MB buffers — see steady state);
  features 0.54; STAND-IN 0.74; rescale up 0.42; compose 0.95; encode x2 6.85;
  host readback 39.3; TOTAL wall 762.9. Steady-state whole GPU pipeline in ONE
  submit (3 runs): 3.886 / 4.107 / 8.930 ms min/median/max — warm ~2-4 ms.
- Full build+run console log: docs/m3-neural.log.
- BLOCKER found & solved (inherited from M2): the DDA acquire took the FIRST
  frame after DuplicateOutput, which on this Arc driver is a BLACK warm-up
  surface (m1dda snapshot_1 luma var=0 range [0,0]; content from snapshot_2 on).
  M2's PASS was unknowingly on that black frame (invert(black)=white==255-original
  masks it). m1's tight loop never noticed because it snapshots frames 60/120.
  Fix in main.cpp: content-checked acquire — keep acquiring and luma-sample the
  staging copy until a non-black frame arrives (6 s fallback accept). Post-fix
  capture sample mean 144.27, metrics PASS.
- What the full-model port still needs: dlssnr-logical.safetensors (649 tensors,
  F16/F32, nr_model.py:735-760) extracted via the external MLX-DLSS extractor;
  the resident graph runtime (xmxres.py Runtime / nr_resident.py block wrappers,
  gemm_resident.comp + resident.comp on XMX — M0 proved 8x16x16 f16 works on B50);
  ~2.3 GiB scratch planning at 720p-class extents; real deterministic_noise() for
  ch0-2; temporal history (channels 7-9 / head.a gate) for live frames.

## M6a result (2026-09-20) - safetensors weights resident on Arc B50: PASS

- Built dlss5/m6-weights-loader/ (standalone console app, raw Vulkan C++17,
  deps: vulkan-1 + dxgi only): main.cpp + CMakeLists.txt + build.cmd. NO
  DDA/present plumbing — pure loader+bench. Own safetensors reader (8B LE
  header len + hand-rolled JSON cursor; validates __metadata__.format,
  decoded_tensor_count vs header, per-tensor shape*eltsize==offsets span,
  8+hdr+data==file size). Whole-file read into RAM (291 MB).
- File: work/mlxw/dlssnr-logical.safetensors — 649 tensors (F16:579, F32:70),
  291,511,674 data bytes (278.0 MiB), ALL under blockN.layerM.* (0 non-block).
  71 blocks (block0..block70). File load 381 ms.
- Device pick: DXGI adapter[0] LUID 00000000:000095a2 == Vulkan device[0]
  (TWO same-name "Intel(R) Arc(TM) Pro B50 Graphics" Vulkan devices again:
  95a2 + 116cc — LUID match used, not index/name). Driver 101.8805, api 1.4.348.
  Heap: 16,206 MB device-local; weights use 278.03 MB = 1.72%.
- Upload: ONE device-local VkBuffer 291,535,872 B (278.03 MB, 256B-aligned
  per-tensor offsets recorded in name->tensor map) via host-coherent staging;
  71 per-block submit+fence copies for timing.
  Staging memcpy 79.8 ms total; GPU copy 142.6 ms (~1.95 GB/s incl. submit
  overhead; per-block min/med/max 0.147/0.433/16.169 ms; stage
  0.008/0.434/6.815 ms). Cold weight-residency cost ~0.6 s end-to-end.
- Verify (--verify): readback of block0.layer0.weight1 (F16 [32,128], 8192 B),
  block10.layer0.attn_scale (F32 [4], 16 B), block35.layer2.attn_scale
  (F32 [32], 128 B) — all 3 byte-IDENTICAL to file after device round-trip.
- Stats (--stats, manual F16 bit-decode): block0.layer0.weight1 n=4096
  mean=-0.001992 std=0.176710 [-0.625,+0.5625]; block35.layer0.weight
  [1024,4096] n=4.19M mean=-0.000081 std=0.031234 [-0.203,+0.203];
  block35.layer4.projection_weight [1024,1024] n=1.05M mean=-0.000037
  std=0.016593 [-0.203,+0.219]. No NaN/Inf, not all-zero, means ~0,
  std in/near the 0.02-0.2 band -> ALL PLAUSIBLE. (NOTE: task-named
  block35.layer0.projection_weight does NOT exist in this dump; layer0's
  matrix is named block35.layer0.weight. Loader prints a note and stats
  both real candidates.)
- Block/dims signature (feeds M6b graph design) — 4 architecture zones:
  - blocks 0-4 (stem-in, 32ch): qkv[32,96], proj[32,32], weight1[32,128],
    weight2[128,32], attn_bias[1,64,64], cos_skip[32], attn_scale[1];
    block0 adds input_adapter_weight[16,32].
  - blocks 5-22 (encoder, 64/128/256ch, GQA heads=ch/32): qkv[C,3C],
    proj[C,C], ffn MoE-branch: ffn_expand_weight[H,4,H,32,32],
    ffn_branch_projection_weight[H,4,32,32], ffn_output_projection_weight[C,C];
    some blocks add weight0[C,2C] (skip adapter). attn_bias[H,64,64],
    attn_scale[H] with H = C/32 (1,2,4,8 heads).
  - blocks 23-38 (bottleneck, 512ch, H=16): split into layer roles —
    L0 first_projection_weight[512,512] + group_expand_weight[8,64,256] +
    group_project_weight[8,256,64]; L1 weight3[512,512] + ffn_cos_skip[512];
    L2 qkv_weight[512,1536] + attn_bias[16,64,64] + attn_scale[16];
    L3 projection_weight[512,512] + attn_cos_skip[512]; blocks 31-38 use the
    fused form: L0 weight[1024,4096], L1 weight[4096,1024], L2 qkv[1024,3072],
    L3 attention_scalar[1], L4 proj[1024,1024] (24 MB/block, the bulk);
    block30 is the 512->1024 transition (has both group* and weight[512,1024]).
  - blocks 39-70 (decoder, mirror of encoder): block39 conv_weight[1024,512]
    + inp_upsample_sin[512] (channel remap); blocks 40-47 = 512ch group-attn
    form; 48-65 mirror 22-5 descending (256/128/64ch, some add weight0[2C,C]
    + sin[C]); 66-70 = 32ch head, block70 adds out_conv_weight[16,4],
    out_gain[16,4], blend_scale[1], inp_merge_cos/sin[32].
  - U-Net total: ~192 MB in blocks 31-38 (the 8x 24 MB blocks) + ~62 MB rest.
  - attn_scale is the ONLY F32 dtype (70 tensors); everything else F16.
    Matrix contract per metadata: K-by-N, output = input @ weight.
- Full build+run console log: docs/m6a-loader.log (gitignored, force-added).
- Blockers: none. Exit 0.

## M7 (2026-09-20) - Vulkan graph prototype: rounding kernel + global block 31: PASS

Full detail: docs/m7-proto.md. Project: dlss5/m7-graph-proto (exe m7proto.exe,
golden.py compare; isolation: isolate.py, quant.py, quant2.py forensics).

- M7a rounding unit kernel: PASS - 100.0000% bit-exact vs CPU (F16C RNE) on all
  4 contract functions (half_round, e4m3, gate_activation, e4m3(gate)) over
  5,063,530 values (all 65536 f16 patterns + boundary vectors + 4M random).
- M7b global block 31 (288 tokens, C=1024, H=32, full MHA + branched FFN):
  GPU run completes; steady-state 1.664 ms/block (20-iter avg) = 4.56 TFLOP/s
  effective; GEMM-only 3.245 ms = 2.34 TFLOP/s. Reproduced across runs (one
  5.067 ms cold-clock outlier discarded).
- Golden compare: 12/14 tensors pass; numerics verdict PASS with 2 documented
  exceptions. Run-2 q16/k16/v16 failure (~1.4% bitmatch) was a golden.py
  COMPARE bug - it flattened q16/k16/v16 in (H,T,D) order while GPU dumps are
  token-major (T, head*32+d). Isolation proves cosine.comp 100.0000% bit-exact
  given identical proj. After fix: h 99.9996%, q16 99.9780%, k16 99.9854%,
  v16 99.9810%, probs 99.9887%, attended16 99.8054% bitmatch; fp32 mean-rel
  1.5e-5..5.9e-3, max-abs <= 1.6% of scale.
- Exceptions (both = fp32 GEMM accumulation-order noise flipping e4m3
  boundaries; float64 control shows the strict elementwise max-rel<2%% floor is
  unsatisfiable by ANY fp32 GEMM): scores max-abs 0.234 (7.8%% of scale,
  0.06%% of elems; downstream probs 99.9887%% bit-exact) and block16 99.07%%
  bitmatch / 99.86%% within-1-e4m3-step (>1-step flips confined to |v|<=0.11,
  abs <= 0.031). Strict all-tensor pass would require bit-identical GEMM
  accumulation - out of scope by design.
- Full-graph go/no-go: 4.56 TFLOP/s -> 460 GF/frame = ~101 ms/frame (~10 fps),
  marginally beyond the design honest envelope of 30-90 ms. GO for building the
  full 71-block graph with planned mitigations (fused epilogues - cosine_v at
  1.447 ms is the top target, barrier elision, render_scale 0.4); NO-GO for
  30 fps until those land.
- Blockers: none. Reference (dlss-nr-on-intel) untouched, read-only as required.
