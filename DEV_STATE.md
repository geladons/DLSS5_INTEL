# DEV_STATE.md - where we are (updated 2026-09-21 ~23:20 PDT by Kimi)

## One-line status
PREVIOUS "FIXED + VERIFIED" CLAIMS FOR BUGS #1-#3 WERE WRONG - they held only in
bounded synthetic tests, NOT in the owner's real usage. Owner 22:25: colors
still broken, trails, stalls, COLORED LINES and other artifacts. Real-scenario
repro (23:05, evidence: dlss5\m1-frame-capture\build\Release\out_live3\
snapshot_5.bmp) confirmed at least two REAL defects:
  (A) THE OVERLAY HIDES ALL NORMAL WINDOWS: it is an OPAQUE TOPMOST FULLSCREEN
      window - Notepad opened during the test is INVISIBLE (it opens UNDER the
      overlay; only windows with their own topmost flag, e.g. Task Manager
      "always on top", the Kimi window, render above). The owner's "zалипание/
      clicks pass through but nothing shows" IS THIS: their new windows open
      behind our overlay and the DDA capture can never see them. --refresh-ms
      just re-renders the same stale content more often - it does NOT fix this.
      This is ARCHITECTURAL, not a gate bug. Product options for the next
      agent: auto-drop the overlay below other windows on foreground change /
      interactive mode toggle / per-window mode (WGC) instead of fullscreen.
  (B) COLORED LINES + BROKEN COLORS DURING MOTION: the live snapshot shows
      short colored horizontal streaks (green/yellow/magenta) at fixed y bands
      (y~270, 550, 780, 970 at 1440p) IN THE PRESENTED FRAME, and color/structure
      errors while content moves. Hypotheses:
      - fbcancel safety clamp +/-48/255: a window move is a +/-255 delta; the
        presented frame only absorbs <=48/255 per processed frame, so during
        and after motion the screen shows partially-tracked smeared content
        ("цветопередача нарушена, шлейфы"). The clamp is a divergence bound,
        but it makes REAL changes arrive distorted over multiple frames.
      - row-DC in the residual: headpack removes the GLOBAL per-channel mean
        only; a per-token-ROW bias survives, hpfilter (S=32 box) does not kill
        row-scale DC -> integrates on moving pixels (resgate w=1 there) ->
        colored horizontal lines at fixed rows. Next test: extend headpack DC
        removal to per-row, or subtract a per-row mean of the head output
        before compose; re-run the live scenario.
WHAT STILL HOLDS from the earlier work: the accumulation defenses do prevent
slow global drift on STATIC content (100-frame test, DDA A/B). The ghost-cursor
fix is real (no painted cursor = no permanent ghosts). But those were the
minor issues. THE MAJOR OWNER PAIN (A + B) IS OPEN.
Bug #4 (imperceptible enhancement) is real but secondary next to (A)/(B).

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

## BUG #1 PARTIAL FIX - build/verify of the mechanism only (13:25-13:31 PDT)
> NOTE 23:20: this fixed the SETTLE GATE stall only. It does NOT fix the
> owner's stale-screen scenario, whose real cause is (A) in One-line status
> (overlay hides windows). Keep the patch (it bounds staleness) but do not
> claim bug #1 fixed.
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

## OWNER UX BUGS - STATUS AS OF 23:20 PDT (CORRECTED; earlier "FIXED" claims
## were synthetic-test-only - see One-line status (A)/(B))
1. STALE SCREEN - NOT FIXED (misdiagnosed): the true cause is the opaque
   topmost fullscreen overlay hiding normal windows (see One-line status (A)).
   The 5b80cda refresh fix only bounds how long the overlay shows its own stale
   content; it cannot show windows that open behind it.
   A/B verified with the gate forced closed (--settle-thresh 255): refresh 800
   -> ~1 Hz forced cadence; refresh 600000 -> stall reproduced. The only
   difference is the new flag. NOTE the forced cadence is ~1 Hz (not 1.25)
   because the loop blocks in AcquireNextFrame(1000 ms) between iterations.
2. BLUE TINT - PARTIALLY FIXED, owner's color complaint OPEN. The slow GLOBAL
   accumulation on static content IS fixed (verified). But the owner's
   "цветопередача нарушена" + COLORED LINES in real use are a DIFFERENT defect:
   row-scale DC in the residual (headpack removes the global per-channel mean
   only) + the +/-48/255 fbcancel clamp distorting real motion (see (B) in
   One-line status). Both hypotheses are testable: per-row DC subtraction in
   headpack; adaptive/full-pass clamp for large real deltas.
3. MOUSE TRAILS - PARTIALLY FIXED. The PERMANENT ghost grid (painted cursor
   never erases) is fixed (005086b, DDA-verified). Motion smearing remains via
   the clamp path ((B)) - presented content lags real changes by multiple
   frames while deltas exceed 48/255.
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
- ENV REGRESSION (2026-09-21..22) - RESOLVED WITH CAVEFATS: root cause was the
  VS Setup.Configuration COM discovery: its CLSID/ProgID registration is
  ABSENT from every registry view (vswhere and COM enumeration return 0
  instances even though _Instances state.json is valid - verified after a
  FULL BuildTools reinstall which also produced a fresh instance 8350b0c9
  that vswhere STILL cannot see). The old toolset files were merged from
  BuildTools.bak into the registered instance dir; build.cmd WORKS again
  (VS generator + cached CMakeCache) and now auto-falls back to
  NMake+vcvars64 (build-nmake\ + sync to build\Release) for cold configures.
  Backups: BuildTools.bak, _vsbt_backup\, _vsbt_*.cmd/.ps1/_*.log helpers.
  If cmake ever reports "instance is not known to the Visual Studio
  Installer" again, the COM registration broke anew - do not reinstall,
  the NMake fallback in build.cmd covers it.
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
-0. SUPERSEDES item 0: owner 22:25 CORRECTED the 22:16 feedback - "nothing
   changed" meant EVERYTHING IS STILL BROKEN: colors, trails, stalls, COLORED
   LINES. Item 0's magnitude analysis stays valid for bug #4 but is NOT the
   top priority. TOP PRIORITIES from the real-scenario repro (23:05,
   out_live3\snapshot_5.bmp = ground truth, and One-line status (A)/(B)):
   (A) OVERLAY HIDES NORMAL WINDOWS - architectural; needs a product decision
       (auto-lower on foreground change / interactive-mode toggle / WGC
       per-window mode). Until then the owner's "open window" UX cannot work.
   (B) COLORED LINES + clamp-distorted motion - testable fixes: per-row DC in
       headpack; adaptive fbcancel clamp (full-pass on large real deltas,
       bounded accumulate for the echo). Repro method: RUN-DEMO + open/move
       a window + m1dda snapshots (see Evidence).
   FIRST-HYPTHESIS ANALYSIS for the next agent (quantified from this session):
   - headpack gain = 0.2, and compose applies the vendor 0.25 factor ->
     NET residual scale = 0.05. Measured post-DC head4 std ~0.017
     ([dbg] head4 ch*: std=0.0172/0.0115/0.0179) -> residual RMS at the
     screen ~0.017*0.05*255 ~= 0.2/255 per frame. Invisible by construction.
   - resgate 4/255 zeroes the residual on everything that changes less than
     4/255 per frame -> on the owner's static photo nearly ALL pixels are
     gated off.
   - The pre-fix runs that looked "visible" (mean|d| 8-17/255, changed 27-40%)
     had NO DC removal, NO gain 0.2, NO resgate - they were also the runs that
     drifted blue. Current pipeline is ~10-50x more conservative.
   - SAFE vs UNSAFE knobs: headpack DC-removal and hpfilter are what kill the
     BLUE DRIFT - keep them. GAIN and RESGATE are the invisibility knobs.
   RECOMMENDED EXPERIMENT SEQUENCE: (a) measure residual magnitude on DYNAMIC
   content (game/video) with the current build; (b) --strength 2.0 run + DDA
   A/B crops; (c) if still weak, raise headpack gain 0.2 -> 0.5..1.0 (net
   0.125..0.25) and/or relax resgate to 2/255, re-run the 100-frame
   accumulation test each time to confirm drift stays dead; (d) owner A/B.
1. OWNER RE-TEST dynamic content: RUN-DEMO.cmd during a game via Moonlight.
2. Repair VS BuildTools registration (re-run bootstrapper) so build.cmd works;
   until then use _build_msb.cmd (MSBuild on the cached vcxproj).
3. Perf fusion pass (30 fps path: cosine_v fusion et al.); M5 zero-copy debug;
   game-mode window targeting; quality/temporal pass.
