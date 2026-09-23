# HANDOFF_M10.md - DLSSNR chain perf (M10), state as of 2026-09-23 ~12:10

## One-line status
M10 in progress: chain at 1920x1088 went 1415 ms -> 1059 ms/frame (0.70 ->
0.92 fps) in one session. Big win: gather_residual warp-per-token (10.8x).
Key discovery: window-attention support-kernel time is NOT in-kernel - it is
the ~1260 full-arena bar() drains per frame. Next levers ranked below.

## Commits (this session)
- 454209d M10 pass 2: ChainProf per-dispatch profiler + m11d --bench +
  gather_residual warp-per-token (chain 1415->1059 ms).
- 083eda2 M10 pass 3: cosine_win vec4 + softmax lane-parallel-shared
  (perf-neutral; kept as the right kernel shape for a post-barrier-fix world).
- 65fc5a7  DEV_STATE.md updated with the negative-result theory.

## Tooling (all game-free, benches only)
- Build: dlss5\m11d\_build_inc.cmd (incremental NMake over build-nmake cache;
   sets VULKAN_SDK itself). Full: dlss5\m11d\build.cmd.
- Bench: dlss5\m11d\build-nmake\m11d.exe --bench 20 1920x1088
  [--prof out.csv]  -> median wall/gpu/chain + per-dispatch CSV.
- Profile: ChainProf (dlss5\chain\prof.*), EngineConfig.profPath; marks in
  every ChainRecorder dispatcher (pm() helper), CSV rows
  frame,fam,note,m,n,k,gx,gy,gz,us. Deltas are serial-stream attribution;
  family totals are trustworthy, per-shape TF/s noisy where dispatches
  overlap (Q/K/V barrier-deduped groups).
- Validation (MANDATORY after any shader/chain change):
  m11d --selftest work\_m9b_cmp\native_crop.bmp  -> out\live_*
  Compare: live_head.bin reshaped [-1,16] vs golden_head.bin [-1,4],
  ch0-2 meandiff MUST stay 0.008265 / 0.007924 / 0.008648 (m8b level);
  live_featV.bin vs golden_features.bin maxdiff 1 f16 ulp (noise channels).
  Compare with the internal managed python (numpy), NOT work/_m9b_cmp/
  cmp_live.py (it points at the stale m8b-live out dir).

## Measured 1080p split (extent 1920x1088, median of 20, prof v6)
gemm 410 ms | smaxw 205 | cosw 184 | ew 44 | part 43 | trans 40 | pool 40 |
gather 38 | upm 37 | smaxg 13 | front-end (decode/features/featpack/
compose/encode/headpack) ~9. Frame total 1059 ms.
GEMM shape/rate notes: fat convs (2088960,128,32) & (522240,128,32) at
~9.9 TF/s; (130560,128,64) 16.4; (32640,128,128) 48; per-shape attribution
inflated where M10-pass-1 barrier dedup lets dispatches overlap. Reference
HW f16 peak per DEV_STATE ~30 TF/s (verify - never independently measured).

## THE finding: support-kernel time = bar() drain tax
recorder.cpp ChainRecorder::bar() inserts a compute->compute memory barrier
over ALL 8 arena chunks (VK_WHOLE_SIZE each) between EVERY dispatch pair
(~1260/frame). Three independent kernel rewrites of smaxw/cosw cutting
in-kernel work 5-30x moved NOTHING (205/183 ms stable). gather moved 10.8x
only because its own sector waste was 16x. Conclusion: latency/ALU-bound
kernels are pinned under the barrier tax; L2 is effectively flushed between
all dispatches.

## Next levers (ranked, detailed plans)
1. BARRIER SCOPING (biggest, riskiest). Replace bar() with a dependency-
   precise barrier: only the arena chunk(s) the PREVIOUS dispatch wrote and
   the NEXT one reads. Arena knows chunkOf(offset) (arena.h/cpp). Traps:
   - Scratch buffers (oG2/oG3/oWIN/oPROJW...) are REUSED across blocks and
     across kernels - consecutive dispatches often alias the same chunk, so
     chunk-level scoping may collapse back to full barriers in the hot path.
     Mitigation: give hot scratch slots per-block double buffers, then scope.
   - Races are SILENT (no validation failure on a lucky run): validate with
     repeated selftest (>=5x) + 60-frame stress bench + compare head dumps
     bit-for-bit across runs (variance = race).
   - Alternative cheaper variant: reduce chunk count touched by bar()
     (barrier only chunks referenced by the surrounding dispatches) via a
     per-dispatch "touched chunk mask" plumbed through pm()/dGemm/etc.
   Expected: several hundred ms/frame if the tax is halved.
2. GEMM K-LOOP PIPELINING (gemm.comp): the K loop (TK=16) does
   coopMatLoad(a/b) then MMA with no prefetch - load latency fully exposed;
   1 subgroup per workgroup, fixed 16x32 tile. Fix order: (a) software-
   pipeline the K loop (prefetch k+1 while MMA k - accumulation order
   unchanged -> bit-exact); (b) 2-4 subgroups per workgroup on distinct
   M-tiles (share B via L2, overlap load/MMA by construction); (c) per-shape
   tile variants via the existing GEMM_RN mechanism. Expected 2-4x on the
   410 ms gemm total. Validate after each step.
3. (skip) fusing window-attn tail ops - gather already 38 ms.

## Hard gotchas (do not rediscover)
- gather C is 32/64/128/256/512 by level; any "C<=128" assumption kills the
  process: wpw=0 -> GPU TDR -> process vanishes with EXIT CODE 0 right after
  the last printf, looks exactly like a hang inside processFrame. Check with
  `cmd //c "... & if errorlevel 1 (echo FAIL) else (echo OK)"` - NEVER
  %errorlevel% (parse-time expansion lies).
- ALWAYS launch m11d/benches DETACHED (python subprocess Popen with
  DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP, stdout to a log file) and poll
  the log. Foreground GPU hangs wedge the tool; tool timeout kills take the
  build children down too.
- git bash lies about child exit codes; taskkill eats /PID (use cmd //c).
- exe under test: dlss5\m11d\build-nmake\m11d.exe (spv staged next to it;
  shader single source of truth dlss5\m8b-live\shaders - after editing,
  rebuild m11d, the .spv copies update automatically).
- One logical commit, ASCII-only, English comments; DEV_STATE.md after each
  pass; weights (work\mlxw\) never in git; Sunshine (pid ~7784) untouchable.

## Session-start prompt for the next agent
See the Russian prompt block at the bottom of this file's history or use:
"continue M10 from HANDOFF_M10.md; lever 1 = barrier scoping, lever 2 = GEMM
pipelining; validate every change with --selftest vs goldens; bench with
m11d --bench 20 1920x1088 --prof".

## Honest playability framing
0.92 fps after pass 2. Even lever 1+2 together plausibly land ~400-600 ms
(~2 fps). 15-30 fps needs ~20-40x and will NOT come from kernel work alone.
When levers 1-2 land, bring the owner the structural checkpoint: precision
experiments (fp16 accumulation with re-baselined tolerances), or accept
cinematic-demo mode with the CTRL+ALT=X pause hotkey workflow.
