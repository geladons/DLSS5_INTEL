# HANDOFF_M10.md - DLSSNR chain perf (M10), state as of 2026-09-23 ~15:40

## One-line status
INCIDENT RESOLVED - it was never the GPU/host: 78a5d28's sparseBinding
device-feature enable silently corrupts large dense allocations on this Arc
driver; fixed in 2ec9135 (feature now gated behind D5C_SPARSE=1), selftest
PASS x3 md5-identical to the 3482274 golden. M10 pass 4: barrier-scoping
lever CLOSED with measurements. Baseline was 1083 ms/frame at 1920x1088;
gemm K-loop pipelining landed it at 1044-1070 ms (~3-4%, bit-exact) - the
Arc compiler already overlapped most load latency. Next gemm lever: bigger
per-subgroup tile (RN=4 -> 16x64) for the N%64==0 shapes.

## Barrier autopsy (pass 4, measured - do not re-litigate)
Tax at 1080p = 1083 - 807 = ~276 ms/frame (~1260 bar() calls, ~3 buffers).
Probe matrix (chain ms, throwaway bar() variants, bench 20):
  3 dense buffers whole-size (production):            1083
  2 dense buffers whole-size (CHUNK_CAP 3800):        1078  (broken numerics
      were later traced to the sparseBinding FEATURE, not the cap size)
  1 dense buffer whole-size 2.9 GB (chunk0 only):      797  (correctness-broken probe)
  1 dense buffer 256 B:                                813
  0 barriers (dense):                                  807  (NO execution-drain component)
  1 sparse buffer whole-size 6.6 GB (D5C_SPARSE=1):   1075
  0 barriers (sparse):                                 995  (sparse access costs ~190 ms)
Conclusions:
- Cost is per-buffer-OBJECT sync (~140 us x extra buffers x bars), size-free,
  but ONLY a single DENSE buffer is cheap; 2 dense buffers pay full tax.
- One dense buffer for the whole arena is impossible (6.6 GB > 4.29 GiB
  allocation cap). Sparse single buffer kills the tax but pays it back in
  access throughput. NET-ZERO - kept behind D5C_SPARSE=1 as a retestable.
- Dependency-precise scoping would gain NOTHING: removing barriers entirely
  (807) ~= tiny-barrier (813) - there is no drain to remove, and with >=2
  buffers any barrier costs the same regardless of range/count details.
- What the tax was hiding: the REAL in-kernel per-frame split at 1080p
  (tiny-barrier probe, prof v6): gemm 382, cosw 169, ew 44, part 43,
  pool 39, upm 38, gather 37, trans 37, smaxw 17 (pass-3 smaxw kernel was
  fine - barrier masked it), smaxg 1.4, front-end ~10. SUM ~808 = the floor.
- Gotcha: per-dispatch CSV timestamps WITHOUT barriers are 50x garbage on
  this driver (timestamps need the barrier ordering). Never profile no-bar.
- The only remaining barrier-adjacent win would be <4.3 GB arena (single
  dense buffer): needs ~2.3 GB scratch shrink (L0 attention batching etc) -
  deferred, high effort, race-audit risk.

## Correctness fixes (this pass, validated before the GPU incident)
1. REVERTED pass 3 (083eda2) cosine_win/softmax shaders: numerically broken
   (head meandiff 0.66, uncorrelated). Proven via torch arbitration: running
   work/_ref_test/dump_torch_f1.py on work/_m9b_cmp/native_crop.bmp reproduces
   golden_head.bin BIT-EXACT (torch==goldens; the live chain was the liar).
   Shaders now at the 454209d state; validation PASSes with the exact
   expected 0.008265/0.007924/0.008648, 5x selftest head dumps md5-identical.
2. spv staging trap FIXED STRUCTURALLY: d5c_stage_shaders used POST_BUILD
   copies (run only on exe RELINK). Shader-only commits never restage -> the
   pass-3 selftest ran the PREVIOUS shaders and "validated" them. Now the
   copies are OUTPUT-based custom commands in a <target>_spv staging target
   the exe depends on (reconfigure happened; works).
3. Helpers: dlss5/m11d/_run_detach.py (detached launcher + log poll; usage
   python _run_detach.py LOG TIMEOUT "expected" -- EXE args...; the decisive
   signal is the expected line in the log, exit codes lie after TDR) and
   dlss5/m11d/_validate.py (numpy compare vs goldens, ASCII).

## Measured 1080p split (extent 1920x1088, median of 20, prof v6) - SUPERSEDED by the real in-kernel split above
gemm 410 ms | smaxw 205 | cosw 184 | ew 44 | part 43 | trans 40 | pool 40 |
gather 38 | upm 37 | smaxg 13 | front-end ~9. Frame total 1059-1083 ms.
NOTE: this table was BARRIER-DRAIN attribution, not kernel cost. The barrier
tax lands on whichever family follows the biggest writes. Real kernel costs
are in the autopsy section.

## Next levers (ranked, after GPU recovery)
1. GEMM K-loop pipelining (gemm.comp, 382 ms real): no-prefetch K loop,
   1 subgroup/workgroup, 16x32 tile. (a) software-pipeline prefetch k+1
   (accumulation order unchanged -> bit-exact), (b) 2-4 subgroups on distinct
   M tiles, (c) per-shape tiles via GEMM_RN. Fat convs (2088960,128,32)
   ~9.9 TF/s of ~30 peak. Expected 2-4x on 382 ms -> ~600-700 ms frame.
2. cosw 169 ms real: pass-3-style vectorization DONE RIGHT this time (with
   the staging fix the selftest actually tests changes now). Verify per-shape
   in the prof CSV before/after; smaxw's 17 ms shows the shape to aim for.
3. Middle class ~236 ms (ew/part/pool/upm/gather/trans, all ~35-45 us x ~1240
   dispatches): latency-bound small kernels; fusion candidates (part+cosw?
   ew into gemm epilogue?) - bigger design work.
4. Honest framing for the owner: floor ~808 ms + perfect levers 1-3 plausibly
   lands ~450-550 ms (~2 fps). 15-30 fps needs ~20-40x and will NOT come
   from kernel work - precision experiments (fp16 accumulate, re-baselined
   tolerances vs torch) or cinematic-demo mode remain the structural options.

## GPU incident (RESOLVED 2026-09-23 ~15:30 - NOT hardware)
~13:00 PDT 2026-09-23: chain numerics went nondeterministically wrong on
every configuration. featV stayed golden-exact, benches stayed fast/stable,
garbage varied run to run. Suspects eliminated with evidence: shaders
(fresh compile md5-identical to staged spv), device selection (both apps
pick LUID 9799), input (hostImg FNV-1a stable), host RAM (torch goldens
bit-exact). m8b-live verify ALL PASS throughout - GPU was always healthy.
TRUE ROOT CAUSE: 78a5d28 enabled the sparseBinding VkDevice FEATURE
whenever the queue family supports it. On this Arc driver (101.8805)
merely enabling the feature silently corrupts large DENSE allocations.
A/B: 3482274 (feature off) selftest PASS x3 md5-identical; 78a5d28 FAIL
(meandiff 0.65-1.93); 78a5d28 + feature gated behind D5C_SPARSE=1
(2ec9135) PASS x3, md5 f1e17ec9 = the 3482274 golden. The earlier
"3800 MB CHUNK_CAP" attribution was WRONG (comment in arena.cpp
corrected). Owner's host/vfio was never at fault.
NOTE: the earlier "rolled-back trees also fail" observation was a false
test - the rollback was not accompanied by a verified rebuild (the
stale-binary trap again). Always rebuild + check exe mtime + md5 the
dumps before believing an A/B.

## Hard gotchas (do not rediscover) - additions this pass
- STALE-SPV TRAP (fixed, but understand it): any shader-only change now
  restages via the <target>_spv dependency, but OTHER consumers of
  d5c_stage_shaders must reconfigure to get the fix. If selftest numbers are
  bit-identical across a shader change, suspect staging, not magic.
- CHUNK_CAP above 3500 MB: was blamed for silent numeric corruption -
  WRONG, the real trigger was the sparseBinding device feature (see the
  incident section). 3800 MB may be fine with the feature off; retest
  before raising the cap.
- CSV profiler timestamps are garbage without intervening barriers.
- Validation arbitration procedure when live != golden: run the torch
  reference (work/_ref_test/dump_torch_f1.py <bmp> <outdir>, needs
  PYTHONPATH=work/mlx-dlss/python + .venv) - torch==golden means live is the
  liar; torch!=golden means stale goldens.
- Device enumeration order is NOT stable process-to-process here; two
  same-name B50s enumerate; D5C_DEV_SKIP=N selects the N-th.

## Commits (this pass)
- (pending) revert pass-3 shaders + spv staging fix + helpers + docs
- (pending) sparse arena opt-in (D5C_SPARSE), sparseBinding enable,
  D5C_DEV_SKIP device select, chunked upload

## Previous status (2026-09-23 ~12:10) - M10 pass 2/3 state

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
