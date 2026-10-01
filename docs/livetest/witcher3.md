# Live Test — The Witcher 3 (DX11 x64, in-process path)

Date: 2026-09-30 ~18:43–18:49 local. Host: remote Windows PC via windows-mcp.
Target: `D:\SteamLibrary\steamapps\common\The Witcher 3\bin\x64\witcher3.exe` (DXVK d3d11/dxgi deployed, manager config mode=dx11 arch=x64).
Purpose: verify the previously-crashing x64 in-process engine path (fix: missing weights/shader env -> chain exit(1) inside game).

## Verdict: **PASS** (all steps)

| Step | Result | Evidence |
|------|--------|----------|
| 1. Pre-flight | PASS | No witcher3.exe running. nr_layer_win.log baseline offset = 31376 bytes. m12_dxgi.log noted (size=175412, mtime 15:58:26 — old). Manager pythonw PID 14072 alive. Bundle/dlls/weights all present. |
| 2. Detached launch | PASS | `%TEMP%\launch_witcher3.cmd` set all env vars, `start ""` spawned exe; witcher3.exe PID 19604 @ 18:43:18. |
| 3. Startup / 3-min poll | PASS | Process stayed alive entire window. In-process engine load logged ~15 s in. Frames processed continuously. |
| 4. 60 s soak | PASS | Frame counter 666 -> 792 across the soak; process alive, Responding=True. |
| 5. Overlay CTRL+ALT+G | PASS | Held combo (~1.1 s via keybd_event) opened panel ON TOP; game did NOT minimize. Second held combo closed it; game kept rendering. |
| 6. Cleanup | PASS | `Stop-Process -Id 19604 -Force` (verified path first); gone. Manager untouched. |
| m12_dxgi.log untouched | PASS | size=175412, mtime=15:58:26 identical before/after — correct, DX11/DXVK path not the dx12 proxy. |

## Layer log evidence (new lines since offset 31376)

Engine activation (x5 init blocks — game rebuilt swapchains during intro; hotkey 2nd+ registrations harmlessly returned 1409):

```
[nr_layer] live: every 1 present processed
[nr_layer] active; port=48990 trigger=(none) capture=(off) every=0
[nr_layer] hotkeys armed: CTRL+ALT+X pause/resume, CTRL+ALT+Q layer off
[nr_layer] static reuse on: exact-repeat frames reuse the last processed result
in-process DLSS 5 active (TCP bypassed, scale=1.00 gain=1.65 blend=1.00 sharpness=0.80)
[nr_layer] framegen: enabled, in-process x64 available, cadence every 2
...
[nr_layer] swapchain 1920x1080 format 37, 3 images
[nr_layer] bypass interception for internal process: dlss5_inprocess
```

Processed + framegen frames (first frames ~18:44:50):

```
[nr_layer] dlss5_framegen_transfer failed (-1); presenting native frame   <- transient first-frame hiccup, recovered
[nr_layer] in-process processed 1920x1080 (frame 2)
[nr_layer] framegen generated 1920x1080 (frame 3, fg #1)
[nr_layer] framegen generated 1920x1080 (frame 5, fg #2)
...
[nr_layer] in-process processed 1920x1080 (frame 1772)
```

Final counters this session: 368 `in-process processed` lines, 12 `framegen generated` lines before stats mode,
last `fg stats: presents=600 processed=300 generated=299 native=1` — perfect every-2 cadence (1 native = the transient).

## Visual notes

- Game ran windowed ~1920x1080 centred on the 2560x1440 desktop; DLSS 5 Manager window overlapped top-left — normal.
- Sequence seen: photosensitivity warning -> legal text -> sponsor logos -> CD PROJEKT RED cinematic -> intro cinematic (village crowd scene, "Space Skip" prompt). Space taps advanced through screens.
- Overlay panel rendered in-game, centred: "PAUSE PROCESSING", target `DX11 game`, Render Scale slider, Frame Generation checked, NVIDIA Phase30 Vendor Tuning (Intensity 1.65, Local Tone/Structure 1.00, Color Strength 0.90), footer "Direct In-Process (DX11 game, 0 ms lag)" and "CTRL+ALT+G - hide the panel". Frame froze while open ("freeze the frame while this panel is open" checked).
- Game image visibly stylized (blue/painterly grade) = neural processing active on live frames.

## Issues / notes

- `pause hotkey unavailable (1409)` / `off hotkey unavailable (1409)` on 2nd+ layer init blocks — ERROR_HOTKEY_ALREADY_REGISTERED, harmless (first registration owns the hotkeys).
- One transient `dlss5_framegen_transfer failed (-1); presenting native frame` on the very first fg frame; recovered, counted as the single `native=1` in fg stats.
- No launcher needed bypassing — witcher3.exe launched directly into the game.
