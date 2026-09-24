# HANDOFF: DX9 present-path injection (GTA IV validated) - 2026-09-23

## State
- DX9 (32-bit DXVK + 32-bit m11 layer -> m11d) works LIVE in GTA IV CE.
- Before/after proof: dlss5/m11-layer/dumps/before.png vs after.png
  (1920x1080 menu frame, meandiff 9.31, 51.7% pixels moved >8).
- Dump tooling: NR_LAYER_CAPTURE (frame sent) + NR_LAYER_CAPTURE_OUT
  (processed result) env vars on the layer; raw = 16-byte header + BGRA.
- Chain perf in-game: ~850 ms/frame at 1920x1080 (matches the ~890 ms
  budget on this GPU; slideshow ~1 fps is expected in live mode).

## What killed GTA IV earlier (postmortem)
- APPCRASH in SYSTEM d3d11.dll, NOT the layer. Cause: DXVK x32 dxgi.dll
  copied into the game folder; GTA IV loads system d3d11 (videos/EVR) and
  system d3d11 + DXVK dxgi is an unsupported mix.
- Rule: for D3D9 games copy ONLY d3d9.dll from DXVK. The removed dll is
  backed up at dlss5/m11-layer/dxvk/x32/dxgi.dll.gta4.bak (do NOT restore).

## Layer controls (live mode)
- CTRL+ALT+X pause/resume (passthrough, native fps), CTRL+ALT+Q layer off.
- Env: NR_LAYER_LIVE=N (every Nth present processed), NR_LAYER_NOPATCH=1
  (bisect arm), NR_LAYER_PORT, NR_LAYER_CAPTURE / _OUT.
- Held result is tied to one swapchain (holding_chain); device resets safe.

## m11d
- TCP 127.0.0.1:47990, log dlss5/m11d/_daemon_gta4.log (this session).
- Lazy engine init per frame size (~31 s first init; ~2 s/frame at
  2560x1440 when m8blive shares the GPU).
- Start: detached only, via python dlss5/m11d/_run_detach.py.

## Next steps (ranked)
1. Gameplay A/B: drive into the game world (pause hotkey makes menus
   usable), capture before/after in motion, check temporal stability of
   the re-blit hold path (live_every>1).
2. UI mask mode (NR_LAYER_UI_MASK=1) for menus/HUD held-still pixels.
3. NR_LAYER_SYNC=semaphore ring (present-semaphore path) to cut the
   double queue idle; measure fps gain.
4. GTA SA (also DX9): same recipe, verify no launcher D3D11 conflict.
5. Perf: chain is ~850 ms; the 51 ms-class target needs the m8b
   optimizations, not this path.

## Prompt for the next agent (paste as the opening message)
---
Project DLSS5_INTEL (C:\Users\AI\Desktop\DLSS5_INTEL): real DLSS 5
(71-block DLSSNR graph) on Intel Arc Pro B50 in a VM. Read DEV_STATE.md
and docs/HANDOFF_M10.md + docs/HANDOFF_GTA4_DX9.md first.

DX9 injection path is LIVE-validated in GTA IV: 32-bit DXVK (ONLY
d3d9.dll in the game folder - never dxgi.dll, system d3d11 crashes on
it) + 32-bit m11 Vulkan layer (nr_layer_win32.dll, registered in
HKCU Vulkan ImplicitLayers) -> m11d on 127.0.0.1:47990. Layer has
CTRL+ALT=X pause / CTRL+ALT=Q off hotkeys in live mode
(NR_LAYER_LIVE=1, setx'd globally). Chain ~850 ms/frame at 1080p.

Continue from docs/HANDOFF_GTA4_DX9.md "Next steps": first do the
gameplay before/after in GTA IV (capture via NR_LAYER_CAPTURE and
NR_LAYER_CAPTURE_OUT on the layer, convert raw 16-byte-header+BGRA to
PNG), then UI mask mode.

Hard rules: NEVER stop Sunshine; game/GPU binaries only detached via
python dlss5/m11d/_run_detach.py; ASCII-only in .cmd/comments; kill
games via cmd //c taskkill; git at "C:\Program Files\Git\cmd\git.exe",
docs gitignored (add -f). m8blive overlay self-respawns - do not fight
it, it shares the GPU (that's why m11d frames take ~850 ms).
---
