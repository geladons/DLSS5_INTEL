# HANDOFF_ECHO_DEGRADE.md - live overlay feedback degradation (OPEN, 2026-09-23)

## Symptom (owner report, demo session 2026-09-23 ~17:00)
Overlay demo (m8blive --echo-free 0, video mode) on GTA4 / desktop:
- Right after launch the improvement is clearly visible (clean first frames).
- Then the presented frame stops tracking the desktop: clicks register,
  windows update underneath, but the overlay shows a stale image that slowly
  DEGRADES. "No working capture->process->present mechanism" - only the
  first frame(s) are right.

## What is NOT the cause (eliminated)
- Chain numerics: selftest PASS x3 bit-exact (md5 f1e17ec9); verify frames
  {10,30,60} ALL PASS in the demo log itself (mean|final-native| ~0.35/255,
  no drift) - so fbcancel subtraction is aligned and the chain input is a
  sane frame, not garbage.
- spv/shaders: fresh compile md5-identical (M10 incident lesson).
- Display-mode changes: solved same-day (self-respawn, see below).

## Mechanism (analysis)
--echo-free 0 (the ONLY owner-visible mode: the owner watches through
the Sunshine/Moonlight DDA stream, and WDA_EXCLUDEFROMCAPTURE overlays
are invisible there)
keeps the overlay INSIDE its own capture. fbcancel subtracts lastPresented
so the chain sees the live delta; the compose then ACCUMULATES a bounded
(+/-12/255) processed delta onto lastPresented.
The failure mode is structural: the 71-block chain is a full-frame
denoiser, but the accumulate path feeds it residual deltas and re-applies
the result every ~0.9-2 s forever. On a static screen deltas ~ 0 (stable).
On a live desktop (cursor wiggle-idle, window updates) every micro-change
is denoised and accumulated again -> progressive over-processing =
degradation. --wiggle-idle 1 (needed to keep DDA alive on a static screen)
makes it worse: the loop never settles, so accumulation never parks.
Evidence in _gta4_demo2.log: RESUME frames spike fbmean 184/73/95/255
(reseed garbage for a frame or two, settles in 5-10 frames); steady-state
fbmean ~0.6 while the owner watches quality decay - delta metrics stay
small because the echo subtraction itself is fine; the decay is in the
chain output being re-denoised, not in the delta.
Earlier --novideo health runs could never see this: no overlay = no echo.

## Why "verify ALL PASS" coexists with a degrading screen
Verify reads the chain's own buffers (final vs native readback), not the
long-term temporal behavior of the accumulate loop on a live screen.

## Ranked next steps
1. CHEAP EXPERIMENT (do first): demo WITHOUT --wiggle-idle on a static
   screen. Expect: settle gate parks the loop -> NO degradation. This
   confirms the accumulate-on-micro-deltas mechanism. If confirmed, the
   demo recipe becomes: pause the game (static) -> one clean processed
   frame -> parks. Check why settle gate did not park with wiggle off
   before (settle-thresh, --settle-thresh CLI exists).
2. STOP-LOSS for live content: freeze accumulation when the residual is
   below a threshold for N frames (park, show last good frame), resume on
   real change. Bounded work in main.cpp compose/fbcancel path.
3. STRUCTURAL FIX (the real answer for games): stop capturing the screen
   at all - inject into the game's present path:
   - DX12: m12-dxgi dxgi.dll proxy (WORKS on GTA5, M12a).
   - DX9 (GTA4, GTA SA): 32-bit DXVK + the m11 Vulkan implicit layer
     (nr_layer_win, VALIDATED on vkcube) feeding m11d. No screen capture
     -> no feedback loop by construction, game-locked resolution, and the
     layer sees the pre-present frame. This was already noted as "M12b"
     in earlier session notes.
4. Do NOT chase this inside --echo-free 1 (capture-excluded) mode: it is
   invisible in the Moonlight stream, useless for the owner.

## Same-day fixes already landed (context)
- Self-respawn on display-mode change (m8blive main.cpp): W/H now pin
  from the live DDA mode at startup; recoverAccessLost respawns the
  process when the output size changes; child self-logs to
  out\m8b_respawn.log; 2 s pre-spawn settle. Verified live both directions
  (2560x1440 <-> 1920x1080), exactly one respawn per flip, ~890 ms/frame
  chain at 1920x1088. (Owner's "everything freezes after opening the
  game" was ACCESS_LOST + stale pinned geometry + a respawn bug:
  relative argv[0] -> CreateProcess error 2; non-inheritable log handle
  -> blind child. Both fixed.)
- m10: sparseBinding device-feature corruption fixed (2ec9135); gemm
  K-loop pipelining (34be0ce, bit-exact, ~3-4%).

## Key locations
- Echo/fbcancel/compose: dlss5/m8b-live/main.cpp (search fbcancel,
  lastPresented, echoFree, compose, settle gate, --settle-thresh).
- Respawn: main.cpp recoverAccessLost + D5C_RESPAWN self-logging in main().
- Demo logs: dlss5/m8b-live/_gta4_demo2.log (video mode, RESUME spikes),
  _gta4_demo3/4.log (respawn tests), build\Release\out\m8b_respawn.log.
- Mode-flip test helper: dlss5/m8b-live/_setres.ps1 W H.
- m11 layer (Vulkan present injection): dlss5/m11-layer + dlss5/m11d.
- DX12 proxy: dlss5/m12-dxgi (GTA5-proven).
