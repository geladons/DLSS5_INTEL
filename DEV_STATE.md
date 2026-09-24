# DEV_STATE.md - where we are (updated 2026-09-23 ~17:05 by Kimi)

## One-line status
M10 pass 4 (2026-09-23 ~12:15-13:30): the barrier question is MEASURED TO DEATH
and the original "barrier scoping" plan is dead - see HANDOFF_M10.md "M10 pass 4".
Two real fixes shipped: (1) pass-3 shaders (cosine_win/softmax) were NUMERICALLY
BROKEN and their "validation" was an artifact of a stale-spv POST_BUILD trap
(now fixed structurally in d5c_stage_shaders); reverted, exact golden meandiffs
restored. (2) Barrier tax measured: ~276 ms/frame at 1080p, but it only
disappears for exactly ONE dense VkBuffer (impossible: arena 6.6 GB > 4 GiB
allocation cap) - sparse single buffer is net-zero (kills the tax but costs
~same in access throughput), 2-buffer dense is no better than 3.
INCIDENT RESOLVED (~15:30, it was never the GPU/host): 78a5d28 enabled the
sparseBinding VkDevice FEATURE, and on this Arc driver (101.8805) merely
enabling it silently corrupts large DENSE allocations - selftest head
meandiff 0.65-1.93 vs 0.008, nondeterministic, featV golden-exact, bench
normal. A/B proof: 3482274 (feature off) PASS x3 md5-identical; 78a5d28
FAIL; 78a5d28 + feature gated behind D5C_SPARSE=1 (2ec9135) PASS x3, md5
f1e17ec9 = the 3482274 golden. The "3800 MB CHUNK_CAP" suspicion was wrong
(arena.cpp comment corrected). Host/vfio exonerated - owner's call was right.
Chain perf unchanged (1083 ms @1080p baseline; 1044-1070 after gemm
pipelining). OPEN: live-overlay feedback degradation on --echo-free 0
(owner demo 2026-09-23 evening: first frames clean, then the accumulate
loop re-denoises its own output and the picture degrades; screen capture
-> process -> present is broken as a LIVE mechanism) - full analysis and
ranked fixes in docs/HANDOFF_ECHO_DEGRADE.md. Structural answer for
games = present-path injection (m12-dxgi for DX12; 32-bit DXVK + m11
layer for DX9), not screen capture.

## M10 pass 4 (2026-09-23 ~12:15-13:30) - barrier autopsy + two fixes + GPU incident
0. Reproduced baseline: --bench 20 1920x1088 -> chain median 1083 ms (handoff
   said 1059-1063; run-to-run spread ~30 ms covers it). All benches detached
   via dlss5/m11d/_run_detach.py (new helper, Popen DETACHED + log poll).
1. Probes (throwaway bar() variants, perf-only):
   - 256-byte single barrier:      chain 813 ms  (-270 ms)
   - NO barrier at all:            chain 807 ms  (no execution-drain component!)
   - 1 dense buffer whole (2.9 GB): chain 797 ms (single barrier free at ANY size)
   - 1 sparse buffer whole (6.6 GB): chain 1075 ms (net-zero vs 1083 baseline)
   - 0 barriers on sparse:         chain 995 ms (sparse access itself costs ~190 ms)
   - 2 dense buffers (3800 cap):   chain 1078 ms (no gain) + SILENTLY BROKEN
     numerics (see incident) - do not raise CHUNK_CAP above 3500 MB.
   Conclusion: tax = per-buffer-object sync (~140 us x 2 extra buffers x ~1260
   bars); no dependency-scoping win available (no drain); single dense buffer
   impossible at 1080p; sparse is a wash -> lever 1 CLOSED with measured data.
   The real in-kernel split (from the tiny-barrier probe, trustworthy): gemm
   382 ms, cosw 169, ew 44, part 43, pool 39, upm 38, gather 37, trans 37,
   smaxw 17 (pass-3 kernel rewrite DID work - it was the barrier masking it),
   rest ~12. NOTE: CSV timestamps WITHOUT barriers are garbage on this driver
   (50x inflated) - never profile a no-barrier build.
2. CORRECTNESS FIX 1: commit 083eda2 (pass-3 cosine_win/softmax rewrite) is
   numerically broken: selftest head meandiff 0.66 (uncorrelated vs torch;
   verified by re-running the torch reference on work/_m9b_cmp/native_crop.bmp
   with work/_ref_test/dump_torch_f1.py - torch reproduces golden_head.bin
   BIT-EXACT, so goldens are fine and the live chain was wrong). Root cause of
   the false pass-3 "validation": d5c_stage_shaders used POST_BUILD copies
   which only run when the exe RELINKS; pass 3 touched shaders only -> stale
   spv -> the selftest validated the OLD shaders. Identical metrics were the
   tell (again). Fixed structurally: copies are now OUTPUT-based custom
   commands feeding a <target>_spv staging target that the exe depends on.
   Shaders reverted to the 454209d state; validation PASS with the exact
   expected meandiffs 0.008265/0.007924/0.008648, 5x selftest bit-identical.
3. PERF EXPERIMENT (kept, opt-in): single sparse VkBuffer for the whole arena
   (arena.cpp chainAllocSparse, env D5C_SPARSE=1). Net-zero at 1080p on this
   driver; retest if the Arc driver improves. Sparse also required
   VkContext to enable sparseBinding and the queue family to carry
   VK_QUEUE_SPARSE_BINDING_BIT (both wired, with fallback). D5C_DEV_SKIP=N env
   picks the N-th enumerated device (two same-name B50s enumerate; order is
   not stable across processes).
4. INCIDENT (RESOLVED ~15:30): ~13:00 chain numerics went nondeterministically
   wrong on EVERY config while featV stayed golden-exact and benches stayed
   fast/stable. Suspects eliminated with evidence: staged spv (fresh glslang
   compile md5-identical), device selection (both apps on LUID 9799), input
   (hostImg FNV-1a stable across runs), host RAM (torch goldens bit-exact);
   m8b-live verify ALL PASS throughout - GPU always healthy. TRUE CAUSE:
   78a5d28's sparseBinding device-feature enable corrupts large dense
   allocations on this driver. Fixed in 2ec9135 (feature gated behind
   D5C_SPARSE=1); selftest PASS x3 md5 f1e17ec9 = 3482274 golden. Lesson:
   the earlier "rolled-back trees also fail" was a false A/B - the rollback
   never rebuilt the exe (stale-binary trap). Always verify exe mtime +
   dump md5 before believing an A/B. Next: control bench 20 @1080p, then
   GEMM pipelining.

## Previous status (2026-09-23 ~10:10) - M12a live on GTA5

## One-line status
M12a LIVE ON GTA5 ENHANCED: dlss5\m12-dxgi dxgi.dll proxy ships DX12 present
frames to the m11d daemon (real 71-block DLSS 5 chain) IN THE REAL GAME.
2026-09-23 ~10:00 run: 195+ frames processed at 1920x1080, chain ~1.3 s/frame
(~0.7 fps slideshow, M12_LIVE=4), no crash, no deadlock. Runtime PAUSE added:
%TEMP%\m12_pause.flag (dlss5\m12-dxgi\_pause.cmd / _resume.cmd) - while the
flag exists every frame passes through untouched (full fps, zero work), so
the owner can walk to gameplay paused and A/B before/after live. GTA5
anti-tamper FORBIDS swapping the swapchain vtable pointer (process dies with
a replaced vtable-copy object, even with M12_DISABLE=1) - m12_hook now
patches the vtable ENTRIES IN PLACE (VirtualProtect/write/restore). BattleEye
BLOCKS loading dxgi.dll from the game dir (Blocked loading of file) - owner
disabled BE in the launcher; Rockstar's mod check complains but launches.
M12b is DEAD (no GTA5.exe anywhere - only Enhanced installed). Plan B if
Enhanced breaks: Cyberpunk 2077 (DX12, no anticheat). Owner hotkeys ship:
CTRL+ALT=X pause toggle, CTRL+ALT+Q full detach (in-place vtable restore).

## M12a live session (2026-09-23 ~09:00-10:10)
1. First real-game run: game CRASHED at startup with the vtable-copy hook
   (worked in m12_test). Bisected with M12_DISABLE=1 (hook loaded, no
   patches): STILL crashed -> GTA5 Enhanced kills the process when the
   swapchain vtable POINTER differs from the original (anti-tamper checks
   the object, not just file integrity). Fix: IN-PLACE vtable patching -
   VirtualProtect the page, overwrite Present/Present1 entries, restore
   protection. The object and its vtable pointer stay byte-identical.
   Passed the anti-tamper (game reached Present #480+).
2. Second run: BLACK WINDOW, no CPU/GPU load, game alive. Root cause:
   all four hooked_CreateSwapChain* thunks in m12_hook.cpp did
   EnterCriticalSection(&g_cs) but never LeaveCriticalSection on ANY exit
   path - m12_test is single-threaded so it never showed; GTA5's render
   thread wedged on g_cs forever. Fixed: Leave on every return path.
3. Third run: CLEAN PASS. Log: `processed 1920x1080` every ~1.4 s,
   `present #N ... flags 0x200` flowing, m11d `frame N 1920x1080 in ~1.3 s
   (chain ~1.25 s)`. Backbuffer is fmt 87 (B8G8R8A8) - no HDR pass-through
   hit. Game presents via Present (which=0 path).
4. BattleEye: with BE on, service log shows
   `Blocked loading of file: "D:\Grand Theft Auto V Enhanced\dxgi.dll"`.
   Owner disabled BE in the launcher settings. Rockstar launcher then warns
   about mods but starts the game. NOTE: online play will need BE off too;
   GTAO is off-limits with the proxy deployed regardless.
5. Runtime pause (owner request): %TEMP%\m12_pause.flag checked every
   onPresent (GetFileAttributesA, ~us). Flag present -> full passthrough,
   transitions logged ("processing PAUSED/RESUMED"); on resume the stale
   processed frame is dropped so live game pixels show until the next
   capture. _pause.cmd / _resume.cmd for the owner; Kimi can also just
   touch/rm the flag. Lets owner walk to gameplay at full fps, then flip
   before/after at will.
6. Gotchas added: taskkill from git bash eats /PID (use cmd //c wrapper);
   GTA5 holds dxgi.dll locked while running (deploy fails Access denied);
   setx M12_DISABLE "" cleanup needs a launcher restart to take effect.
## M12a build session log (2026-09-23 ~00:50-02:00) - SUPERSEDED where noted
0. SUPERSEDED: m12_hook originally used a per-object vtable COPY (item 1
   below). GTA5 anti-tamper kills any swapchain whose vtable pointer was
   replaced -> m12_hook was rewritten to IN-PLACE entry patching (see live
   session above). Architecture, env keys, client/log modules unchanged.
1. New module dlss5\m12-dxgi (OOP per AGENTS.md): m12_log (kernel32-only
   file logger, %TEMP%\m12_dxgi.log, M12_LOG overrides), m12_client (TCP,
   m11 wire protocol verbatim, port M12_PORT default 47990), m12_dx12
   (M12Dx12Processor: readback/upload staging on the GAME's command queue
   captured at CreateSwapChain (pDevice IS the ID3D12CommandQueue), fence
   waits, resize handling, HDR formats logged + passed through), m12_hook
   (per-object vtable COPY - never writes .rdata; factory entries
   10/15/16/24, swapchain Present(8)/Present1(22 via IDXGISwapChain1 QI)),
   m12_exports (CreateDXGIFactory* forwarded to system32 dxgi.dll by fully
   qualified path; 19 internal DXGI helpers stubbed - never called by
   games). Env: M12_LIVE (default 4), M12_DISABLE=1, M12_DUMP=path.bmp.
2. GOTCHAS COSTING TIME (do not rediscover):
   - CRT stdio (fopen/fprintf) inside this proxy CRASHES in DllMain context
     as a game dependency dll. All logging is CreateFile/WriteFile. (The
     exact CRT failure was never root-caused; suspect ucrt init order.)
   - Git Bash tool LIES about child exit codes: plain `foo.exe` reported
     127 while cmd-inner %errorlevel% was 0. Always verify via cmd.
   - IDXGISwapChain::GetCurrentBackBufferIndex is on IDXGISwapChain3 - QI.
   - Copy >=48 vtable entries: Win11 factory vtables reach entry 31.
   - OCCLUDED flip window: DXGI's internal retry thread re-presents the
     swapchain at ~60-240 Hz IN-PROCESS -> hooked Present runs without the
     app calling it; captures then re-read our own blit (feedback drift).
     Proxy throttles live captures to >=250 ms intervals; test harness
     artifacts, real games present from their own thread.
   - 47990 ownership: Sunshine listens on 0.0.0.0:47990 (its web port!) -
     m11d binds 127.0.0.1:47990 alongside (Windows permits specific-IP
     over wildcard bind; loopback routes to m11d). A connect to "the
     daemon" when m11d is DOWN reaches SUNSHINE's HTTPS server: connect
     succeeds, exchange then fails. Check `tasklist | findstr m11d`, never
     assume from netstat alone.
   - Game dir ACL: D:\Grand Theft Auto V Enhanced is RX for Users; AI is
     Administrator but the shell is filtered -> copy needs elevation
     (_deploy_elevated.cmd + owner UAC click).
3. Validation rig: dlss5\m12-dxgi\test\m12_test.exe (own DX12 gradient app;
   proxy staged as dxgi.dll in test\run; _selftest.cmd). The in-app final
   readback can wedge (present-park interplay with the retry storm), so
   the decisive check is M12_DUMP: proxy writes its last processed reply
   as BMP; single-pass (M12_LIVE=1000000) dump vs known solid = mean|d|
   7.1/255, center px shift +14/+2/+7, max 19 - correct pixel path both
   directions incl. R/B swizzle.
4. Chain perf at 960x540: ~700 ms/frame (GEMM-bound, matches M10 notes).

## Next steps (order)
0. DONE 2026-09-23 ~10:00: real-game validation PASS (see live session above).
   Remaining owner experience items: effect is subtle at default gain; if the
   owner wants it punchier, restart m11d with --gain up to 2 (add the flag to
   dlss5\m12-dxgi\_start_daemon.cmd) - requires daemon restart, NOT the game.
1. M10 GEMM tuning: chain is ~1.25-1.4 s at 1920x1080 -> 0.7 fps slideshow.
   This is THE blocker for playability. M10 notes exist in repo history.
1a. DONE 2026-09-23 ~11:45 (M10 pass 2 - first blood): per-dispatch profiler
   (ChainProf, dlss5\chain\prof.*, env-free: EngineConfig.profPath / m11d
   --bench N WxH [--prof csv]) measured the REAL 1080p split (median 20
   frames, extent 1920x1088, chain 1415 ms): gather 410 ms (29%), gemm 406
   (29%), smaxw 204 (14%), cosw 174 (12%), ew/part/pool/trans/upm ~215.
   SURPRISE: window-attention SUPPORT kernels are 56% of the frame - not the
   GEMMs (M10 pass 1 barrier work had pointed at GEMM; the truth needed
   per-dispatch data). gather_residual.comp was the worst: scalar per-channel
   loop, one token per thread -> warps strided by C floats (16x sector waste),
   23 ms/call at L3. Fix: warp-per-token mapping (lane -> (token, C/4-vec4
   slot), one warp covers 128/C tokens, C in {32..512}; per-element math and
   order unchanged). gather 410 -> 38 ms (10.8x); chain 1415 -> 1059 ms
   (0.70 -> 0.92 fps). Validated vs torch goldens: head meandiff 0.00827 ==
   the m8b level, features maxdiff 1 f16 ulp (noise ch) - IDENTICAL numbers to
   pre-change. GOTCHAS: (a) gather C is up to 512 (deep fam 0/1 blocks) - the
   first warp-mapping attempt assumed C<=128 -> tpt>32 -> wpw=0 division
   killed the process silently (GPU TDR + exit 0, looks like a hang in
   processFrame); (b) use gl_WorkGroupID, NOT gl_GlobalInvocationID, for the
   warp index. Bench harness: m11d --bench 20 1920x1088 --prof out.csv
   (detached launch + poll the log; foreground GPU hangs wedge the tool).
   NEXT M10 targets (same scalar-disease class): smaxw 210 ms + cosw 182 ms
   (softmax.comp / cosine_win.comp: scalar loads, 2-pass softmax); then the
   gemm 408 ms (K-loop pipelining, multi-subgroup tiles).
1b. DONE 2026-09-23 ~12:00 (M10 pass 3 - NEGATIVE RESULT, theory corrected):
   cosine_win vec4 accesses + softmax lane-parallel-weights-in-shared (both
   bit-exact validated, head meandiff identical 0.008265). ZERO perf change:
   smaxw 205 ms / cosw 183 ms across THREE independent kernel rewrites that
   cut in-kernel work 5-30x. CONCLUSION: the support-kernel time is NOT in
   the kernels - it is the ~1260 full-arena bar() drains per frame (every
   dispatch pair gets a memory barrier over ALL 8 arena chunks, VK_WHOLE_SIZE
   each). Gather's 10.8x worked because its own cost was sector waste (16x),
   overwhelming the fixed tax; latency/ALU-bound kernels cannot move until
   the tax is removed. REAL NEXT LEVERS, in order:
   (a) barrier scoping: bar() only the arena chunk(s) the next dispatch
       actually touches (arena knows chunkOf(offset); scratch buffers oG2/oG3
       are reused across blocks so consecutive dispatches often share chunks -
       needs a dependency audit or it RACES; validate with repeated selftest +
       stress bench). Expected: several hundred ms/frame.
   (b) GEMM K-loop software pipelining + multi-subgroup tiles (gemm 410 ms
       at ~10-60 TF/s effective; the fat conv shapes (2088960,128,32) run at
       9.9 TF/s - ~2-4x plausible while keeping accumulation order ->
       bit-exact).
   (c) fuse the window-attn tail (gather already cheap at 38 ms; skip).
2. M12c polish: async pipeline (capture thread) to unhook the present block,
   per-size engine warmup, HDR (R10G10B10A2) conversion if a game needs it
   (GTA5E ships B8G8R8A8, not hit).
3. M13 one-click manager after owner confirms the path is fun to demo.
   NOTE M12b is dead: no GTA5.exe (Legacy) installed anywhere. Plan B title
   for DX12 path if Enhanced breaks: Cyberpunk 2077 (D:\SteamLibrary, no
   anticheat; drop the same dxgi.dll next to Cyberpunk2077.exe).

## Owner controls (m12 proxy)
- CTRL+ALT+X: pause/resume processing (full fps while paused).
- CTRL+ALT+Q: full detach (vtables restored in place, proxy inert).
- Scripts: dlss5\m12-dxgi\_pause.cmd / _resume.cmd (same pause channel);
  _start_daemon.cmd (m11d); _deploy_elevated.cmd / _undeploy_elevated.cmd
  (UAC). Env: M12_LIVE (default 4), M12_DISABLE=1, M12_DUMP=path.bmp,
  M12_LOG, M12_PORT (47990).
- Owner A/B evidence 2026-09-23 (GTA5E story mode, Franklin house scene,
  1080p, owner-labeled): dlss5\m12-dxgi\evidence\m12a_before.png = original
  game (paused); m12a_after.png = chain-processed. The two captures are
  different moments (phone out in the processed one), so treat as indicative
  A/B, not pixel-matched. Observable in the processed frame: slightly softer
  distant background haze vs the punchier original - consistent with the
  calibrated SUBTLE mean|d| ~2-7/255 effect; judge on matched scenes with
  CTRL+ALT+X flipping, that is what the hotkey is for.

## Previous status (2026-09-23 ~00:10) - M11 handoff state
The REAL 71-block DLSS 5 chain processes Vulkan presents live (m11-layer ->
m11d -> chain module), 2178 vkcube frames on the owner's stream, torch-validated.
Demo stopped, GPU free. Details in docs\HANDOFF_M12.md.

## Previous milestone (2026-09-22 ~23:35)
M11 DAEMON LIVE: the REAL 71-block DLSS 5 chain now processes vkcube presents
through the Vulkan layer - dlss5\m11d (TCP daemon on 127.0.0.1:47990) runs
ChainEngine (new dlss5\chain OOP module) at ~145 ms/frame on the 500x500
cube. Validated vs torch goldens BEFORE going live: 15/16 feature channels
bit-exact (color ch0-2 within 1 f16 ulp on 0.26% of tokens), head meandiff
0.0083 == the m8b-live validated level. Processed cube pixels verified
artifact-free (residual concentrates on logos/edges, x8 diff inspected).
Demo: dlss5\m11d\_demo.cmd (m11d console + vkcube; kill both after).
Next: real game via DXVK (32-bit layer build for DX9 games), then GEMM
perf (chain is ~135 ms at 512x512, ~930 ms at 1344x1088).

Why the pivot (owner call 2026-09-22 ~19:45): overlay mode with --echo-free 0
shows a great first frame then the effect DECAYS each frame (fbcancel eats
the residual through the capture loop) and clicks hit the overlay.
Screen-level processing is the wrong level; the layer sits just above the
driver instead.

## M11 daemon session log (2026-09-22 ~21:30-23:35)
1. dlss5\chain module (per owner code rules): vk_util.h / fp16.h /
   VkContext (vk_context.*) / WeightsStore (weights.*, safetensors + pack +
   fuse-fold + de-swizzle + side tables) / ChainArena (arena.*, slot layout +
   3.5 GiB chunking + BDA) / ChainRecorder (recorder.* + recorder_blocks.cpp,
   14 pipelines + 71-block walk) / ChainEngine (engine.*, absolute echo-free
   front-end: decode->features->featpack->chain->compose->encode->readback).
   All chain code VERBATIM-ported from m8b-live main.cpp (lambdas -> methods).
   Shaders compile from m8b-live\sources (single source of truth).
2. dlss5\m11d: TCP daemon, protocol identical to nr_layer.c (16-byte header
   {magic,w,h,fmt} + BGRA payload, masked variant reads +w*h mask). Lazy
   engine init per frame size, alpha preserved (vendor nr_daemon.py:88
   parity; encode forces opaque for the overlay use-case), held-still UI
   mask pixels pass through. --selftest file.bmp = one-shot validation mode
   (frame index 1, dumps out\live_*).
3. Validation (m11d --selftest native_crop.bmp 1309x1070, extent 1344x1088):
   features ch3-15 BIT-EXACT vs golden, ch0-2 maxdiff 1 f16 ulp (0.26% of
   tokens, meandiff 4.6e-07); head ch0-2 meandiff 0.0083 == m8b's own
   validated deviation. LIVE cube reply: mean|d| ~2/255 color, geometry and
   colors intact, residual rides logos/edges (x8 diff PNG inspected).
4. Gotchas hit: cmake function directory-scope vars are invisible at the
   call site (d5c_stage_shaders silently staged nothing -> spv dir now
   travels via a GLOBAL property); LNK1104 when rebuilding over a running
   m11d.exe (kill first); two "Arc Pro B50" Vulkan devices enumerate today
   (LUID 968b / 11646) - headless engine takes the first coopmat-capable,
   works, but pinning by LUID may matter later.
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
