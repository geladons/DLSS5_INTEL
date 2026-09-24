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
1. DONE 2026-09-23 ~18:00 (NEGATIVE result, mechanism nailed instead): demo
   WITHOUT --wiggle-idle on a live desktop (_exp_nowiggle.log, 100 frames):
   the loop does NOT park - every present is itself a desktop update (DDA
   keeps delivering) and forceRefresh (chain ~1.65 s > refreshMs 0.8 s)
   bypasses the settle gate on EVERY frame ("0 settled skips" everywhere).
   fbmean decayed 182 -> 0.00 (capture == lastPresented bit-exact) while the
   per-frame SIGNED residual kept integrating (verify frame 60: R -0.76/255)
   - the degradation is the integral of the chain's zero-delta response
   (network(black+noise) != 0), invisible to both fbmean and verify.
   Removing --wiggle-idle does NOT save the demo.
2. DONE 2026-09-23 ~18:10 (STOP-LOSS SHIPPED): accumulate parking gate in
   main.cpp (overlay loop only). After --acc-park-frames (3) consecutive
   frames with fbmean < --acc-park-thresh (1.0) the accumulation PARKS:
   overlay holds the last good frame, zero chain work (measured: settled
   skips + quiet GPU probes only, 0 drift), resume on est >=
   --acc-resume-thresh (4.0; cursor moves force 1e9) or on a GPU probe
   (upload+fbcancel+stats only, every --acc-probe-ms 4000, no chain/no
   present). Validated live: PARKED -> RESUMED -> re-PARKED cycle
   (_park_test2/3.log); --novideo 31-frame verify ALL PASS. Full details in
   DEV_STATE.md "Accumulate stop-loss session".
3. IN PROGRESS 2026-09-23 ~19:40 (DX9 path validated end-to-end in a
   synthetic app): 32-bit DXVK 3.1.1 + the 32-bit m11 implicit layer
   (x86\nr_layer_win32.dll) -> m11d. Own 32-bit D3D9 test app: 30 frames
   800x600 through the real 71-block chain, ~255 ms/frame. Two loader
   requirements found: no enable_environment in the 32-bit manifest
   (gates the layer off) and an ABSOLUTE library_path (relative fails
   with error 87 in the x86 loader path). Registry: HKCU ImplicitLayers
   is NOT Wow6432Node-redirected; both manifests live in the same key.
   Full details + deploy recipe in DEV_STATE.md "DX9 path (M12c)".
   Remaining: deploy next to GTAIV.exe / gta_sa.exe and confirm in-game.
   DX12 (m12-dxgi, GTA5) was already live.
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
