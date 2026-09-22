# DEV_STATE.md - where we are (updated 2026-09-21 ~20:45 PDT by Kimi)

## One-line status
ALL FOUR owner UX bugs addressed: #1 stale screen FIXED+VERIFIED (5b80cda),
#2 blue tint FIXED+VERIFIED (ec08838 + headpack DC + hpfilter), #3 mouse
trails FIXED+VERIFIED (005086b), #4 enhancement visibility = honest report
below (not a bug - a tuning/expectations item). RUN-DEMO is the owner build.

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

## OWNER UX BUGS - FINAL STATUS (2026-09-21 ~20:45 PDT, all verified on host)
1. STALE SCREEN - FIXED (5b80cda): --refresh-ms forces a full process+present
   at least every 800 ms even when the sparse settle estimate reads "settled".
   A/B verified with the gate forced closed (--settle-thresh 255): refresh 800
   -> ~1 Hz forced cadence; refresh 600000 -> stall reproduced. The only
   difference is the new flag. NOTE the forced cadence is ~1 Hz (not 1.25)
   because the loop blocks in AcquireNextFrame(1000 ms) between iterations.
2. BLUE TINT - FIXED (ec08838, the --resgate patch, PLUS the headpack
   per-channel DC pass and the hpfilter high-pass that were already in).
   Three-layer defense: headpack subtracts the head's per-channel DC (measured
   -0.700 vs design |mean|<0.02!), hpfilter kills low-freq residual, resgate
   zeroes the residual on every pixel whose real change is below 4/255, so a
   static background CANNOT integrate anything by construction.
   VERIFIED 2026-09-21 20:20-20:32: 100-frame run (frame-60 metric = 50 frames
   of potential accumulation) -> signed delta <= 0.08/255, no channel bias;
   DDA ground-truth snapshots of the real screen 45 s apart -> wallpaper clean,
   only +/-1 LSB wobble on high-contrast edges (R/B only, mean -0.02/255, a
   quantization wobble, invisible); no blue cast anywhere. The apparent -0.5
   "drift" seen in BMP diffs earlier was contamination (Task Manager + the
   Kimi window sit ABOVE the overlay and update live; the verify "native" is
   the echo capture, not the true desktop).
3. MOUSE TRAILS - FIXED (005086b). REPRODUCED first: the painted cursor never
   erases (echo-fbcancel algebra = 0 at old cursor pixels) - one wiggle pass
   left a permanent 5x5 grid of 25 ghost cursors (DDA snapshot, _trail_crop).
   FIX: cursor compositing OFF by default; the hardware cursor is drawn by DWM
   above the topmost overlay so the user sees it regardless. Legacy path kept
   behind --cursor-draw. RETESTED: DDA snapshot of the wiggle area is clean;
   --frames 300 verify {10,30,60} ALL PASS; in-app metrics are cleaner too
   (changed 0.4-1.6% vs 6-27% with the painted cursor contaminating).
4. NO VISIBLE ENHANCEMENT - honest report, NOT a bug: the anti-drift defenses
   intentionally make the residual tiny on static content (per-frame mean|d|
   0.03-0.1/255, changed <2% on the owner's static photo). On dynamic content
   the residual is clearly visible (old --frames 200 run: mean|d| 8-17/255,
   changed 27-40%). Tuning path for the owner: --strength 1.5-2.0 for a
   stronger effect on the photo scenario; a proper visual A/B needs game/video
   content in motion (suggest the owner runs a game via Moonlight and compares
   CTRL+ALT+X toggle). A quality/temporal pass was planned anyway.

## OPEN QUESTIONS / WATCH ITEMS
- ADAPTER LUID CHANGED: 95a2 -> 9a8b across a host re-enumeration (seen in the
  20:37 run log). Not a bug - M8c picks by dynamic LUID match - but AGENTS.md's
  hardcoded "LUID ...95a2" is stale. LUIDs are per-boot; never hardcode.
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
- Frame pacing: ADDRESSED by --refresh-ms (forces ~1 Hz processed cadence on an
  idle desktop; no cursor moves needed). External wiggle helper for tests:
  dlss5\m8b-live\_wiggle_loop.ps1 (SendInput SetCursorPos loop, start via
  _wiggle_start.cmd; ui.ps1 move X Y also works).
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
1. OWNER RE-TEST: RUN-DEMO.cmd on the real scenarios (photo in Photos, a game
   via Moonlight). All four bugs have verified fixes in 5b80cda..005086b.
2. Bug #4 tuning: owner A/B with --strength 1.5 / 2.0 on the photo scenario;
   visual A/B on dynamic content (game video); then decide default strength.
3. Repair VS BuildTools registration (re-run bootstrapper) so build.cmd works;
   until then use _build_msb.cmd (MSBuild on the cached vcxproj).
4. Perf fusion pass (30 fps path: cosine_v fusion et al.); M5 zero-copy debug;
   game-mode window targeting; quality/temporal pass.
