# DEV_STATE.md - where we are (updated 2026-09-22 ~21:00 by Kimi)

## One-line status
M11 v0 DONE: the Windows Vulkan layer (dlss5\m11-layer, port of the reference
nr_layer.c) intercepts vkcube's presents, roundtrips frames through a TCP
daemon and writes them back - owner SEES a green rotating cube in the
Moonlight stream. This is the post-overlay architecture: no desktop capture,
no feedback decay, no click-through problems, stream-native visibility.
Next: extract the 71-block chain from m8b-live main.cpp into a module and
stand up the real daemon (m11d), then a real game via DXVK (owner has no GTA
SA; the GTA IV window was a screenshot, not the game).

Why the pivot (owner call 2026-09-22 ~19:45): overlay mode with --echo-free 0
shows a great first frame then the effect DECAYS each frame (fbcancel eats
the residual through the capture loop) and clicks hit the overlay.
Screen-level processing is the wrong level; the layer sits just above the
driver instead.

## M11 v0 session log (2026-09-22 ~19:50-21:00)
1. Ported reference\dlss-nr-on-intel\src\layer\nr_layer.c (1064 lines) to
   dlss5\m11-layer\nr_layer_win.c: pthread->CRITICAL_SECTION, Unix socket->
   TCP 127.0.0.1:47990, access->_access, stderr->%TEMP%\nr_layer_win.log.
   Semantics 1:1 incl. TRANSFER_SRC patch, idle/semaphore sync, live/photo
   modes, UI mask. Built as C (MSVC C++: C2375 on the exported Vulkan names;
   dllexport-after-declaration is a hard error in C too -> .def exports).
   Gotchas: "interface" is an MSVC keyword (windows.h); logf collides with
   the math intrinsic.
2. Manifest VkLayer_dlssnr_win.json: THIS LOADER REQUIRES disable_environment
   (skips the layer without it); enable_environment gating never fired, so
   the layer is always-on unless DISABLE_NR_LAYER=1. Registered via HKCU
   ImplicitLayers (no admin needed). Absolute library_path.
3. vkcube validation: capture-to-file decoded perfectly (500x500 fmt44,
   LunarG cube pixels). daemon_stub.py (green tint) roundtrip: daemon in/out
   means correct, layer logged "processed 500x500" continuously, OWNER
   CONFIRMED green rotating cube in the stream. Local GDI AND local DDA show
   windowed Vulkan flip windows BLACK on this host (MPO) - verify Vulkan
   windows only via the owner's stream or the layer's own readback.

## M9b status (superseded by M11 but still the validation base)
M9B SHIPPED: the live app runs the REAL 71-block DLSSNR U-Net per-pixel at
the vendor-aligned extent of the target window (e.g. 1309x1070 region ->
1344x1088 network). Validated against the torch reference: features
BIT-EXACT, composed output 50.1 dB PSNR vs reference (frame-1 head) /
46.9 dB vs the on-screen verify frame. Root-cause kill: driver-measured
rgba8-on-BGRA texel reversal - decode/fbcancel read R as B; encode writes
identity RGBA. Arena chunked (Arc caps one VkDeviceMemory at ~4 GiB).
Speed: ~800 ms/frame at 1344x1088 (chain-bound; M10 pass 1 = no gain, the
lever is GEMM kernel efficiency, 0.6-2.9 TF/s vs ~30 TF/s hw).

## Owner-visibility fix + expected-effect calibration (2026-09-22 ~19:10)
- ROOT CAUSE of "nothing on screen": the owner watches via Moonlight
  (Sunshine CLIENT CONNECTED, DDA 2560x1440). WDA_EXCLUDEFROMCAPTURE hides
  the overlay from that capture -> invisible since 11:10. Fixed for demos:
  tools\RUN-*.cmd now pass --echo-free 0 (overlay captured, fbcancel on).
  Owner CONFIRMED the overlay is visible again in window mode.
- EXPECTED EFFECT calibration (reference\dlss-nr-on-intel\README.md):
  DLSS-NR is a one-step pixel-space diffusion pass with SUBTLE native-res
  output - measured there: characters darker, fabric texture +22..50%,
  backgrounds untouched, MK1 faces even LOSE detail; "better = taste".
  Our torch-vs-native on the anime wallpaper: mean|d| 6.5/255 overall, 36%
  of pixels >8/255, concentrated on characters (faces, fabric, flowers) -
  the same magnitude class. Live matches torch at ~50 dB, so the pipeline
  does EXACTLY what the reference file does. The NVIDIA marketing demo
  (dramatic face repaint) is NOT what this file does at native res on
  already-clean content. Strongest visible effect = 3D game frames with
  fine shading/fabric, not flat anime JPEGs.

## M10 pass 1 (2026-09-22 ~17:20) - barrier dedup: NO measurable gain
- Removed 2 of 3 barriers between the Q/K/V cosine dispatches in
  recordWindowAttn (dCosW) and recordGlobal (dCosG): all three read the same
  oPROJ buffer and write disjoint oQ/oK/oV slots, so the intermediate barriers
  were pure overhead. ~140 barriers removed of 1403 dispatches.
- Validation after the change: 45-frame run PASS, verify ALL PASS, features
  99.98% bit-exact vs torch golden (3851/23.4M elems, 1 f16 ulp, screen noise),
  head RGB mean|d| 0.0083 (unchanged). Numerics intact.
- Speed: chain 796.8 (pre) -> 799.6 (post) ms median of frames 40-44; the
  16:25 pre-run measured 767.7, i.e. run-to-run spread (~30 ms) exceeds the
  effect. Conclusion: at 1344x1088 the chain is COMPUTE-bound, not
  barrier-bound (dispatch/barrier overhead ~10% at full extent, measured
  ~50-96 us marginal per dispatch+barrier at 576x512 where it dominates).
- Real lever for M10 pass 2: GEMM/kernel efficiency. Per-family rates measured
  at 576x512: stem 0.61 TF/s, head 0.65, decoder 1.24, enc 1.52, bottleneck
  1.71, global 2.89 - vs ~30 TF/s f16 hardware. 2x better GEMM kernels ~= 2x
  chain speedup. That is a kernel-fusion/tuning milestone, not barrier work.
  Deferred; chain is correct and stable at ~1.1 fps for now.

## M9b session log (2026-09-22 ~14:00-16:40)
1. Ported the validated m9-unet chain into m8b-live (shaders m8/pool2,upmerge,
   softmax+nReal; features.comp rewritten to vendor make_features with mirror
   extension at the vendor-aligned extent; compose.comp rewritten to vendor
   compose_head: residual = halfr(head)*0.25*gain, out = clamp(src+res)).
2. Chunked device arena (CHUNK_CAP 3.5 GB, chunkOf/localOff/A per logical
   offset; barriers/descriptors/fill/debug-copies chunk-aware). Fixes
   vkAllocateMemory -2 at >4 GiB (vulkaninfo: maxMemoryAllocationSize =
   0xffff0000).
3. R/B SWAP FIX (the "fundamental error"): layout(rgba8) storage ops on a
   VK_FORMAT_B8G8R8A8_UNORM view are REVERSED by this Arc driver (measured:
   live featV ch4 == reference ch6 exactly, 5000-px probe; corr 1.0000).
   decode.comp/fbcancel.comp now read .x=R .z=B; encode.comp stores identity
   RGBA. After the fix: live features bit-exact vs torch (maxdiff 1 half-ulp,
   noise channels only), head RGB mean|d| 0.008 @ std 0.13.
4. Validation rig: work/_ref_test/dump_torch.py (+_f1 frame_index=1 variant)
   dumps torch goldens per block; cmp_live.py compares live frame-1 dumps
   (out\live_*.bin, written every run at frame 1). m9-unet gained x16/xfeats
   dumps. Gotcha that cost an hour: m8b_native.bmp is written ONLY at verify
   frames {10,30,60} - at --frames 10 it is STALE from the previous run;
   always crop goldens from m8b_native0.bmp (frame 0, every run) instead.
5. Numbers (region 1309x1070 -> extent 1344x1088): verify ALL PASS,
   mean|final-native| ~6-7/255 per channel, changed 99.9%, no drift.
   m9-unet at 576x512 cross-check: torch-vs-m9 43.45 dB, torch-vs-live
   44.44 dB, m9-vs-live 46.16 dB.

## Previous status (2026-09-22 ~11:10)
ECHO-FREE PIPELINE SHIPPED (this commit): the overlay is now excluded from
capture via WDA_EXCLUDEFROMCAPTURE -> DDA sees the TRUE desktop through the
overlay. Root-causes killed at once: wrong colors (feedback equilibrium is
gone), windows hidden behind the overlay (they are captured + presented),
blue-tint/drift class (no accumulate loop in the default path). Freeze class
addressed: bounded swapchain acquire (no more UINT64_MAX parks), hotkey drain
after every present, present pacing cap (--pace-ms, default 66 = <=15 fps).

OWNER: re-test with Desktop\RUN-DEMO.cmd. Expected: normal colors everywhere
(not just Task Manager), windows visible, no freezes; enhancement is SUBTLE
(mean|d| ~0.2/255 on the static wallpaper - see "quality ceiling" below).
Hotkeys: CTRL+ALT+X hide/show, CTRL+ALT+Q quit (now responsive).

## What changed this session (2026-09-22 10:10-11:10)
1. REPRO (owner scenario): notepad opened behind the overlay was invisible
   (Layer A confirmed 3rd time); during motion the loop processed ~1 Hz while
   DDA delivered 140 fps; every frame blocked ~240 ms in submit+present+fence.
2. FREEZE FIXES (main.cpp):
   - swapchain acquire: UINT64_MAX -> 50 ms tries x4 with drainHotkeys()
     between; on persistent NOT_READY the frame is SKIPPED, thread never parks.
   - drainHotkeys() after every present+fence wait.
   - present pacing --pace-ms (default 66): max ~15 processed fps, GPU idle
     between frames -> DWM keeps composition headroom.
3. INPUT CONTRACT (Layer 2a): features.comp channels 0-2 are now the REAL
   vendor deterministic_noise() (uint32-exact port of
   work/mlx-dlss/python/mlxdlss/features.py:87; transcendentals fp32, result
   half-rounded). The old integer-hash stand-in is gone. Also fixed silently:
   shaders/publish.glsl half_round is a NO-OP on this Arc driver (packHalf2x16
   folding) - features.comp/temporal.comp now use a local manual bit-exact
   halfr() (same code as m8/publish.glsl).
4. TEMPORAL PATH (Layer 2b, --temporal 0|1, DEFAULT OFF): features 7-9 carry
   real history from a new bufHist slot; new temporal.comp implements vendor
   compose_temporal (predicted + alpha*(history-predicted), BLEND_SCALE
   0.73974609375) at token res, with per-row zero-mean residual (B1 lesson).
   ON HOLD: at the event-driven ~1 Hz cadence history blending GHOSTS moving
   windows and the stronger residual re-showed row lines (owner screenshots
   10:55). Revisit only with realtime cadence (per-window game mode).
5. ECHO-FREE (the fundamental one): SetWindowDisplayAffinity(hwnd, 0x11).
   When active (default; --echo-free 0 for legacy): fbcancel skipped, decode
   feeds the raw capture, compose/encode run ABSOLUTE (no accumulate), no
   lastPresented mirror. fbmean stays 0.00 by design. Verified: notepad opened
   behind the overlay appears in the PRESENTED frame (out\m8b_frame_30.bmp),
   colors faithful, mean|d| 0.20/255, verify ALL PASS, gpu/frame 66 ms.

## Quality ceiling (honest, open)
The chain runs at a PINNED 12x24 = 288-token grid for ANY content: a 2560x1440
desktop is downscaled to 24x12 (~107 px/token) before the network sees it.
That is far off the vendor's operating point, so the residual is low-frequency
tone junk, not detail: measured head DC -0.7..-1.3 (design expects |mean|<0.02)
which is WHY the per-row DC calibration exists, and why "enhancement" is
invisible on static content. This is the owner's "no DLSS5 improvement visible"
- it is a DESIGN ceiling, not a bug in this pipeline. The real fix is the
per-window mode (WGC capture of a game/video window at render-like resolution
-> adequate token density) per docs\windows-port-plan.md sec.1 + NeuralScreen's
OpenWgc pattern. THAT is the next milestone (Layer 1 proper).

## Numbers (current build, echo-free default)
- Chain ~50-56 ms/frame, e2e ~59 ms, gpu (submit+present+fence) ~66 ms (was
  ~240 ms in the feedback build).
- Verify: --novideo 31 frames ALL PASS (temporal 0 and 1); video 35 frames
  ALL PASS with a window opened mid-run.
- New CLI: --pace-ms N (66), --temporal 0|1 (0), --echo-free 0|1 (1).

## Batch files (desktop + tools\ are in sync now)
- RUN-DEMO.cmd: m8blive --frames 1000000 (all defaults; honest wall text).
- RUN-DEMO-CALM.cmd: + --wiggle-idle 30.
- RUN-DEMO-M4.cmd: legacy stand-in transport demo (unchanged, historical).

## WINDOW MODE SHIPPED (same day, second commit): --window TITLE
Per-window mode: resolves the target by title substring, crops the DDA capture
to its client rect (region = window), the overlay covers ONLY that rect and
follows position moves; resize/close ends the run cleanly; minimize pauses.
Combined with echo-free capture the desktop stays fully usable. Verified live
on the owner's Photos window (1309x1039): defaults = invisible-safe
(mean|d| 0.45/255); --gain 1.0 --colorpass 1 = clearly VISIBLE model effect
(mean|d| 10-15/255, tone/vibrance grade, structure intact, no lines).
IMPORTANT DIAGNOSTIC LANDMINE: out\m8b_processed.bmp is written ONLY at
verify frame 30 - any --frames N < 31 run leaves a STALE pair (cost me two
A/B runs; identical metrics were the tell).
New CLI: --gain F (headpack residual gain, default 0.2). New batch:
tools\RUN-WINDOW-DEMO.cmd (+ desktop copy) - takes the title as %1.

## M9 FINDING (2026-09-22 ~12:00) - THE REAL QUALITY ROOT CAUSE
The vendor network extent is NOT 288 tokens. NetworkGeometry.vendor_aligned
(work/mlx-dlss/python/mlxdlss/features.py:36-40): network extent = the IMAGE
extent aligned to 64, minimum 320 - per-PIXEL features; the graph downsamples
internally (24x12 -> 12x6 are INTERNAL to the chain). Our 12x24 grid = feeding
a 24x12-PIXEL thumbnail: the model literally cannot see faces -> its residual
can only be low-frequency tone junk (the "digital garbage + slight color
change" the owner reports).
VALIDATED WITH NVIDIA'S OWN WEIGHTS via the torch reference (CPU, venv at
work\_ref_test\.venv): owner's photo 1920x1200 -> ref_full.png: 68 s network,
mean|d| 7.3/255, 79% pixels touched = subtle denoise/refine, structure 1:1.
At 512x320: 6.2 s network. Conclusion: the model on flat 2D photos does
REFINEMENT, not the promo face-regeneration (that marketing is in-game DLSS 5
with motion vectors/depth/G-buffer inputs we do not have on the desktop).
Expectation set with the owner via _ref_test\_ffN.png vs _ffP.png crops.
PERF MATH: reference CPU does 164k tokens in 6.2 s; our Vulkan chain does 288
tokens in 50 ms (tiny-grid overhead dominated). At real extents the B50 GPU
should be vastly more efficient per token - a 512x320 window at proper density
is plausibly realtime; a 1309x1039 window (aligned 1344x1088 = 1.46M tokens)
is ~seconds/frame until perf work lands.
NEXT MILESTONE (M9): generalize the chain to arbitrary extents (multiples of
64, >=320): runtime IMG_W/IMG_H, window-attention batching beyond the 8-window
bucket, GEMM M dims, offset arithmetic. Validate vs torch reference on
photo_512 (work/_ref_test) - the harness now exists. Then wire into m8b-live
window mode.

## Next steps (order)
0. M9 (above) SUPERSEDES the old list - extent generalization is THE quality fix.
1. OWNER RE-TEST: RUN-WINDOW-DEMO.cmd anime (Photos window) - judge the
   effect; then Desktop\RUN-DEMO.cmd for the fullscreen path.
2. Token-density study: 288 tokens over a window is still coarse; check
   whether the m8 chain extent is truly pinned or parameterizable (GEMM
   strides derive from TOK; attention is O(TOK^2); m8a goldens are
   288-specific). If it scales: --tokens for denser grids on small windows.
3. True WGC capture (removes the DDA fullscreen dependency), Vulkan-layer
   game attach - per docs\windows-port-plan.md.
4. Perf fusion (cosine_v et al.); M5 zero-copy backlog.
5. temporal re-enable ONLY at realtime cadence.

## Watch items carried over
- LUIDs are per-boot; never hardcode (95a2 -> 9a8b -> 968b observed).
- VS Setup.Configuration COM discovery still broken; build.cmd NMake fallback
  works (used for this commit's builds).
- GDI screenshots cannot see the overlay; DDA now CANNOT see it either (by
  design, WDA_EXCLUDEFROMCAPTURE). Ground truth of the PRESENTED frame =
  verify readbacks out\m8b_frame_{10,30,60}.bmp; ground truth of the DESKTOP =
  m1dda snapshots.
- --wiggle-idle re-arm suspect (DEV_STATE history) - untested this session.
