# DEV_STATE.md - where we are (updated 2026-09-21 13:35 PDT by Kimi)

## One-line status
The full DLSS 5 graph runs live on the Arc B50 and all ENGINEERING milestones
pass. Owner bug #1 (stale screen) now has a BUILT + VERIFIED fix (--refresh-ms,
commit see git log). Remaining owner bugs: blue tint (#2, next), mouse trails
(#3), no visible enhancement (#4).

## Milestones (all PASS unless noted)
- M0 coopmat probe, M1 DDA capture, M2 D3D11<->Vulkan interop (bit-exact),
  M3 neural slice, M4 live overlay (37 fps), M6a weights resident (278 MiB),
  M7 numerics contract VERIFIED (bit-exact vs CPU goldens, benign fp32 exceptions),
  M8a full 71-block chain validated (47 ms/frame), M8b REAL DLSS live (verify PASS),
  M8c Sunshine-aware capture (de2d594, see below).
- BACKLOGGED: M5 zero-copy (deadlock on frame-4; needs VK validation + apitrace).

## M8c (2026-09-21 morning) - committed de2d594 (code) + 60ae26d (docs)
Sunshine-aware capture in m8b-live\main.cpp: Cap struct {device,context,dup,
stagingTex,output,outX,outY}; CapBuild/CapBuildFromPick shared by startup and
ACCESS_LOST recovery. Startup enumerates ALL DXGI adapters/outputs, prints an
inventory table, picks by priority: --output NAME substring > output containing
the cursor (GetCursorPos) > primary > first attached; D3D11 device is created ON
the picked output's adapter. Startup starvation (idle desktop) -> temporary
WiggleThread up to +12s. ACCESS_LOST -> re-enumerate + re-pick + rebuild
(cross-adapter safe: CPU-bridge transport). Verified: 60/30-frame video runs
PASS with Sunshine running; cursor painted correctly (confirmed via DDA snapshot,
NOT GDI - see AGENTS.md).

## UNCOMMITTED WORKING-TREE CHANGE (not built, not tested!)
Settle-gate staleness fix (partial fix for owner bug #1 below), in main.cpp:
- New CLI --refresh-ms N (default 800; clamped >=50).
- Live loop: `tLastProcessed` timestamp + `forceRefresh` bypass in the settle
  gate (line ~2770): full process+present at least every refreshMs even when the
  sparse settle estimate says "settled". Idle cost ~6% GPU (51 ms chain / 800 ms).
TO FINISH: run build.cmd, verify compile, run `runm8b.cmd --frames 30` with
cursor kept moving (or --wiggle-idle 1) -> check docs\m8b-live.log for periodic
processed frames + verify PASS -> commit.

## BUG #1 FIX - BUILT + VERIFIED (2026-09-21 13:25-13:31 PDT)
The 1be426c patch was INCOMPLETE: it used `refreshMs` and `tLastProcessed`
without declaring them (3x C2065) - the commit could never have compiled.
Added: `long refreshMs = 800` decl, `--refresh-ms N` CLI parse + usage line,
`auto tLastProcessed = clk::now()` before the live loop. Nothing else changed.
NOTE: build.cmd itself is currently BROKEN on this host (see VS registration
issue below); the verify build was done by invoking MSBuild.exe directly on the
existing build\m8blive.vcxproj (same cl.exe 14.29 toolset, same flags).
Verification runs (all on Sunshine host, overlay video mode):
1. `--frames 30` + external cursor-wiggle loop: PASS, verify {10,30,60} ALL
   PASS, 0 drops, ~19 fps, signed delta mean ~0 (no color drift at frame 10).
2. `--frames 12` passive run (active desktop): PASS, verify ALL PASS.
3. Mechanism A/B with the gate FORCED always-closed (--settle-thresh 255):
   A) default --refresh-ms 800: 4 processed frames at ~0.8-1.0 s cadence
      despite every loop iteration reading "settled" -> the forceRefresh
      bypass carries the cadence. This is the bug #1 fix working.
   B) --refresh-ms 600000 (refresh effectively off): stall reproduced -
      frame 0 only, settled skips accumulate forever (the old 10:51
      behavior: 867 skips / 6 frames). Only difference = the new flag.
   => bug #1 root-cause fix VERIFIED. Cosmetic note: at forced-refresh pace
      the long AcquireNextFrame waits count as "dropped (DDA timeout)" in the
      summary; that counter is acquire-timeout accounting, not real drops.

## OWNER UX BUGS (from screenshot attachment-20260921-105537, priority order)
Owner test: photo (anime, colorful) in Photos app + RUN-DEMO (all defaults).
1. STALE SCREEN (worst): clicks pass through, but opening a window shows
   nothing. Root cause: settle gate (est mean|d| < 0.5 skips chain+present) uses
   512 sparse samples (every 24th row, 1997-byte stride). A new window in an
   unsampled band reads est=0 -> overlay keeps the old frame forever. The log
   shows "settled (est mean|d|=0.000)" for 100s of skips while this happens.
   PARTIAL FIX in working tree (forceRefresh, see above). Also consider larger
   settleThresh or dirty-rect-driven decisions later.
2. BLUE TINT: whole screen gains a blue cast over time/frames. Mechanism
   documented in shaders\hpfilter.comp: presented(n)=presented(n-1)+residual(n),
   any correlated (DC/low-freq) residual integrates. hpfilter (S=32 box
   high-pass) is ON by default and was believed to fix it; owner still sees blue.
   NOT YET RE-DIAGNOSED on the current build. Next: run with frames flowing,
   diff out\m8b_native0.bmp vs m8b_processed.bmp (both written on processed
   frames) - per-channel signed delta tells the bias; also DDA-snapshot the real
   screen (m1dda) to see the accumulated tint. Suspects: (a) S=32 box too small
   to kill screen-scale gradients; (b) headpack per-channel DC pass (see
   "headpack two-pass" ~line 927) interacting with hpfilter (double subtraction
   -> overshoot); (c) tint in features/compose path, not residual.
3. MOUSE TRAILS/artifacts: cursor ghosting while moving. Likely from the
   fbcancel delta clamp (4*maxDelta=48/255) + compose clamp (12/255) + the
   settle gate skipping frames mid-motion: cursor deltas accumulate partially.
   Expected to improve with the refresh fix (ghosts cleared every 800 ms). If
   not: raise maxDelta (--max-delta) for the cursor path or paint cursor after
   accumulation instead of before.
4. NO VISIBLE ENHANCEMENT: on a static photo the DLSSNR residual is subtle and
   reads as "a color filter", not neural reconstruction. Partly expectations:
   the model was validated bit-exact as a CHAIN, never VISUALLY benchmarked on
   real content. After bugs 1-3: A/B crops (native vs processed from the verify
   BMPs) on game content and on the owner's photo; consider --strength up to 2.0;
   then a proper quality/temporal pass (was planned anyway, see PROGRESS.md).

## OPEN QUESTIONS / WATCH ITEMS
- ENV REGRESSION (2026-09-21 ~13:18): VS BuildTools instance at
  C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools EXISTS on
  disk (VC Tools 14.29.30133, MSBuild 16.11.6 both present and functional)
  but is NOT REGISTERED with the VS Installer anymore: vswhere (3.1.7)
  returns ZERO instances and state.json in
  C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances\eaa86170 is a
  June-18 install snapshot; cmake -G "Visual Studio 16 2019" fails with
  "instance is not known to the Visual Studio Installer". Builds at 09:29
  worked, so registration broke in between. WORKAROUND used for the bug #1
  verify build: MSBuild.exe build\m8blive.vcxproj directly (toolset intact).
  PROPER FIX for owner: re-run the BuildTools bootstrapper (repair), or
  restore the _Instances registration. build.cmd stays canonical.
- --wiggle-idle re-arm suspect: 2026-09-21 ~11:04 run with --wiggle-idle 1 armed
  at startup ("[info] cursor-wiggle generator armed") but the live loop never
  printed "wiggle-idle: no updates ... engaging" despite 40+s of idle, and no
  cursor-forced frames appeared (settled skips continued). Either the re-arm
  branch is not reached or the wiggle thread dies. The startup-wiggle path WAS
  verified historically (M1). Investigate lines ~2620-2676.
- Frame pacing: processed frames need CURSOR MOVES or real desktop changes on an
  idle desktop; runs "stall" at 5 processed frames otherwise. For testing, move
  the cursor externally (workspace\skills\win-desktop-control\scripts\ui.ps1
  move X Y) or use --wiggle-idle 1 (after the open question is fixed).
- RUN-DEMO wall text still says "~12-20 fps"; actual is ~12 fps active, idle 0.

## Numbers (current build, video mode)
- Chain: ~50-52 ms/frame (fe 0.7 / fp 0.0 / chain 50-57 / tail 1.7-2.8),
  e2e processed frame ~54 ms (~18-19 fps potential; live pacing is event-driven).
- fbcancel: max-delta 12/255 (compose), 48/255 (fbcancel clamp), settle 0.5.
- Weights resident 325 MB; DDA acq ~7-13 ms when frames flow.
- Perf path to 30 fps: fusion (M8b note: cosine_v 1.447 ms is the top target;
  chain 47 ms isolated, ~101 ms naive full-graph).

## Evidence / where things are
- Owner screenshot: C:\Users\AI\.kimi_openclaw\workspace\chat-attachments\
  attachment-20260921-105537-f5ee95.png (blue tint visible on photo).
- Live log: docs\m8b-live.log (also PROGRESS.md = milestone log).
- Cursor-verify crop (M8c, DDA method): done via m1dda; methodology in
  C:\Users\AI\.kimi_openclaw\workspace\memory\2026-09-21.md.
- M8c handoff detail: dlss5\m8b-live\out\M8C_HANDOFF.md.
- Agent session memory: C:\Users\AI\.kimi_openclaw\workspace\memory\2026-09-21.md.

## Next steps (suggested order)
1. ~~Build + test the settle-gate refresh patch~~ DONE + VERIFIED (see "BUG #1
   FIX" above). Also repair the VS BuildTools registration so build.cmd works.
2. Reproduce + measure blue tint (bug #2) with BMP diffs and m1dda snapshots;
   fix hpfilter/headpack accordingly.
3. Re-test mouse trails (bug #3) with the refresh fix in; tune clamps if needed.
4. Visual A/B on real content (bug #4), strength study, honest quality report
   to owner; then quality/temporal pass planning.
5. Later: perf fusion pass; M5 zero-copy debug; game-mode window targeting.
