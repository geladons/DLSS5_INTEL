# Live test — GTA IV (Stage-26 verification)

Date: 2026-09-30. Host: bench PC via windows-mcp (Moonlight/Sunshine stream).
Game: D:\games\Grand Theft Auto IV\GTAIV.exe (x86, DX9 -> DXVK -> Vulkan layer -> TCP daemon).

## Verdict: PASS (all steps)

## Step results

1. **Pre-flight — PASS.** No GTAIV.exe / m11d.exe running. Only python processes were the four
   windows-mcp `serve` instances (PIDs 20772, 21212, 21728, 22180) — left untouched.

2. **Manager + daemon — PASS.**
   - `DLSS5 Manager.vbs` started detached; `pythonw` PID **14072** running
     `production\m13\m13.pyw`.
   - `m11d.exe --weights ...dlssnr-logical.safetensors` started detached, PID **20920**.
   - NRCT STATUS probe on 127.0.0.1:48990: sent `{0x5443524E,2,0,0}`, got 16-byte reply
     `magic=0x5443524E ok=1 gainBits=1.575-ish frames=0` — daemon answering before game launch.

3. **Game launch with layer armed — PASS.** Wrote `%TEMP%\launch_gta4.cmd` (cmd sets
   `ENABLE_NR_LAYER=1 NR_LAYER_LIVE=1 NR_LAYER_GAIN=1.65 DLSS5_SCALE=1.0 NR_LAYER_SCALE=1.0
   DLSS5_BLEND=1.0`, `cd` to game dir, `start GTAIV.exe`). Env confirmed passed:
   `%TEMP%\nr_layer_win.log` written immediately with
   `hotkeys armed`, `active; port=48990`, `d5c_chain.dll not found; falling back to TCP socket`,
   `framegen: disabled ... cadence every 1` (NR_LAYER_LIVE=1 honored).

4. **Menu reached — PASS.** GTAIV.exe PID **19736**, `Responding=True`, `MainWindowTitle='GTAIV'`.
   Screenshot shows the GTA IV / Episodes From Liberty City menu filling the 800x600 screen.
   Layer log: `swapchain 800x600 format 44, 3 images`; steady
   `fg stats: presents=N processed=N` (~all presents processed; 112500/112497 by end).
   Note: expected `degenerate swapchain 160x28` line did NOT appear this run — only the real
   800x600 swapchain was created. Brief early transients (`the daemon did not answer` x3,
   `no daemon on 127.0.0.1:48990` x1) self-recovered with
   `daemon answered again; processed output resumed`.

5. **Overlay open (CTRL+ALT+G) — PASS.**
   - First MCP `Shortcut ctrl+alt+g` did NOT trigger (press too brief for the 120 ms
     GetAsyncKeyState poller — see Caveats).
   - Held combo via `keybd_event` (~0.9 s hold) fired: `%TEMP%\m13_freeze.flag` created
     immediately, overlay Toplevel shown.
   - Screenshot: overlay panel (DLSS 5, ~470x710, centered) over the still-fullscreen game.
     Panel showed `Frame frozen. Turn the knob - the ORIGINAL frame is reprocessed with the
     new gain.`, `target: DX9 game`, `Daemon Active (x86) (PID 20920) - gain 1.65, 41 frames`.
   - Game NOT minimized: `GetForegroundWindow()` = GTAIV hwnd 0xE0738 (class `grcWindow`,
     title `GTAIV`, pid 19736, `IsIconic=False`) — the NOACTIVATE invariant held;
     fg stayed on the game (== fg_before).

6. **Freeze + live-reprocess — PASS.**
   - NRCT SETGAIN 3.0 -> `reply magic=0x5443524E ok=1 gain=2.000 frames=41`
     (daemon accepts 0..16 per m11d/main.cpp:448 but the Phase30 controller clamps intensity
     at 2.0 — reply reports the clamped value).
   - Bumped `%TEMP%\m13_reproc.flag`; layer log gained
     `reprocessed the held frame (live knob turn)` lines and
     `frame frozen (800x600); knobs reprocess the original`.
   - Screenshot B: frozen menu frame re-rendered visibly stronger (harsher contrast/sharpening,
     red tint shift on character jacket), NOT black/broken; overlay status updated to
     `gain 2.00, 43 frames`.
   - SETGAIN 1.65 -> `ok=1 gain=1.650 frames=43`; reproc flag bumped again; frame restored.

7. **Overlay close — PASS.** Second held CTRL+ALT+G removed `m13_freeze.flag`
   (hide() -> unfreeze()); layer resumed live processing (fg stats kept climbing);
   screenshot shows plain game menu, no panel.

8. **Cleanup — PASS.** `Stop-Process -Id 19736` (GTAIV) and `-Id 20920` (m11d).
   Both gone; manager pythonw 14072 left running; freeze/reproc flags both absent
   (cleaned by the manager/layer).

## Key log excerpts

`%TEMP%\nr_layer_win.log`:
```
[nr_layer] hotkeys armed: CTRL+ALT+X pause/resume, CTRL+ALT+Q layer off
[nr_layer] active; port=48990 trigger=(none) capture=(off) every=0
[nr_layer] static reuse on: exact-repeat frames reuse the last processed result
d5c_chain.dll not found; falling back to TCP socket
[nr_layer] framegen: disabled, unavailable (requires in-process x64 engine), cadence every 1
[nr_layer] swapchain 800x600 format 44, 3 images
[nr_layer] the daemon did not answer; frame unchanged          (x3, early)
[nr_layer] no daemon on 127.0.0.1:48990                        (once, early)
[nr_layer] daemon answered again; processed output resumed
[nr_layer] fg stats: presents=300 processed=298 generated=0 native=0   ... up to
[nr_layer] fg stats: presents=112500 processed=112497 generated=0 native=0
[nr_layer] reprocessed the held frame (live knob turn)         (x5 around overlay session)
[nr_layer] frame frozen (800x600); knobs reprocess the original
```

`production\logs\m11d.log`:
```
[m11d] frame 1616 800x600 in 188.3 ms (chain 181.8)
```
(daemon steady ~188 ms/frame on the 800x600 BGRA TCP path; ~1600 frames served)

`production\logs\manager_err.log`: no tracebacks this session; last line is the normal
`[config] dropped 2 duplicate game entrie(s)` startup message.

## Screenshots

The windows-mcp `Screenshot` tool returns inline images only (no files saved). Four were
taken: menu baseline, overlay open over fullscreen game, reprocessed frozen frame at
gain 2.0, and post-close live menu. Content described inline above.

## Caveats / follow-ups

- **Hotkey press duration:** the overlay HotkeyPoller polls `GetAsyncKeyState` every 120 ms.
  The MCP `Shortcut` tool's tap was too brief to be caught (no overlay, no freeze flag).
  A ~0.9-1.2 s held combo via `keybd_event` fired reliably. For scripted live tests on this
  host, drive ctrl+alt+g with a held injection, not a tap. (Also note: a raw `SendInput`
  KEYBDINPUT struct with a 24-byte union fails silently on x64 — cbSize must be 40.)
- The in-app `[overlay] shown ... fg_before/fg_after` line goes only to the manager's
  ScrolledText widget, not a file. Foreground was verified externally: fg window remained
  the GTAIV hwnd before and after show (never the overlay, never desktop).
- SETGAIN 3.0 clamps to 2.0 in the Phase30 params (m11d accepts 0..16 but reports the
  clamped intensity). Effect was still clearly visible on the frozen frame.
- `degenerate swapchain 160x28; passing through` did not occur — GTA IV presented a single
  real 800x600 swapchain this run.
