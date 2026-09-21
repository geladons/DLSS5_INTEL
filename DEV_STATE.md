# DEV_STATE.md - where we are (updated 2026-09-21 12:45 PDT by Vasya)

## One-line status
The full DLSS 5 graph runs live on the Arc B50 and all ENGINEERING milestones
pass, but the OWNER's real-world test (2026-09-21 ~10:57, viewing a photo via
RUN-DEMO.cmd) found it UNUSABLE: blue tint, stale screen, mouse trails,
enhancement invisible. These UX bugs are the current work.

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
1. Build + test the settle-gate refresh patch (uncommitted) -> fixes owner bug #1.
2. Reproduce + measure blue tint (bug #2) with BMP diffs and m1dda snapshots;
   fix hpfilter/headpack accordingly.
3. Re-test mouse trails (bug #3) with the refresh fix in; tune clamps if needed.
4. Visual A/B on real content (bug #4), strength study, honest quality report
   to owner; then quality/temporal pass planning.
5. Later: perf fusion pass; M5 zero-copy debug; game-mode window targeting.
