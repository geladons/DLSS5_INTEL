# Red Dead Redemption 2 (DX12, m12 proxy) — live test

**Result: FAIL** — RDR2.exe crashes with the Rockstar "exited unexpectedly" dialog before presenting a single frame. Reproduced on two consecutive launches.

## Setup
- Launched `D:\Red Dead Redemption 2\Launcher.exe` via `%TEMP%\launch_rdr2.cmd` with the full m12 env (M12_LIVE=1, M12_SCALE=1.0, M12_GAIN=1.65, M12_BLEND=1.0, DLSS5_WEIGHTS_PATH / NR_LAYER_WEIGHTS = dlssnr-logical.safetensors, DLSS5_SHADER_DIR / NR_LAYER_SHADERS = production\runtime\m12).
- dxgi.dll (m12 proxy) + d5c_chain.dll deployed in the game folder.
- Log watched: `%TEMP%\m12_dxgi.log`.

## Timeline
- Attempt 1: Launcher pid 12072 attached 19:39:17; RDR2.exe pid 10468 attached 19:39:18. By ~19:40 the game showed an `Error` window: "Red Dead Redemption 2 exited unexpectedly! Please visit https://support.rockstargames.com for more information." Dismissed → both processes exited.
- Attempt 2 (relaunch to rule out a transient crash): Launcher pid 20792 + RDR2.exe pid 22436 attached 19:41:37; identical `Error` dialog ~40 s later; dismissed → both exited.
- Log contained ZERO `present` / `processed` lines for RDR2 across both attempts — the crash happens before (or at) device/swapchain creation, i.e. earlier than the Metro fixed bug (which crashed *on the first processed frame*).

## Evidence — %TEMP%\m12_dxgi.log tail (RDR2 section)
```
[19:39:17.138] dllmain process attach
[19:39:17.139] m12 dxgi proxy loaded (pid 12072, live every 1)   <- Launcher
[19:39:18.377] dllmain process attach
[19:39:18.378] m12 dxgi proxy loaded (pid 10468, live every 1)   <- RDR2.exe
[19:41:37.421] dllmain process attach
[19:41:37.422] m12 dxgi proxy loaded (pid 20792, live every 1)   <- retry: Launcher
[19:41:37.469] dllmain process attach
[19:41:37.470] m12 dxgi proxy loaded (pid 22436, live every 1)   <- retry: RDR2.exe
```
No further lines. Proxy DLL loads fine inside RDR2.exe; the game then dies during init.

## Notes / hypotheses for the dev
- Crash is deterministic (2/2) and pre-swapchain: no `present` lines at all, unlike the silent-exit-after-first-frame bug fixed for Metro.
- Screenshots captured: Rockstar "Red Dead Redemption 2 exited unexpectedly" dialog both times.
- Possible causes to check: d5c_chain.dll side-effects during RDR2's early device enumeration; RDR2 launching Vulkan path (would show no DXGI presents, but wouldn't explain the crash dialog); Rockstar SDK/launcher interplay with a hooked DXGI; missing env inherited by the actual game process (launcher respawns RDR2.exe — env inheritance through Launcher.exe worked for attach, but any CreateProcess flags resetting env would reproduce the old missing-env exit).
- Cleanup done: error dialog dismissed; RDR2.exe and Launcher.exe had already exited on their own — no stray processes left, nothing killed by hand.

## Follow-up (attempt 3, same binary)
- RDR2.exe pid 20392 stayed alive ~1h with 0:00:29 CPU and no window: proxy attached, `CompatValue` stub calls logged, zero `present` lines — a silent pre-swapchain hang rather than the crash dialog. Killed by hand after path/PID verification; Launcher.exe exited on its own.
- Root cause candidate found and fixed in `24a54c4`: the proxy patched the swapchain class vtable in the module's shared .rdata. Hooking is now a per-object private vtable swap (vptr rewrite only). `m12_test` passes end-to-end (presents intercepted, in-process chain processed 960x540 frames). **Retest pending.**

## Root cause found — wrong transport for a Vulkan game (attempts 4-6)

- A/B test: with `dxgi.dll` removed from the game folder RDR2 boots to the
  main menu normally (2.8 GB RSS, "Running"). With the proxy present it
  stalls at ~190 MB / ~29 s CPU, no window, deterministically.
- Instrumented forwarders showed CreateDXGIFactory1/2 calls succeeding —
  the stall is later, during init, with the foreign dll loaded. A copy of
  the REAL signed `dxgi.dll` in the folder boots fine, so the binary itself
  is not blocked; our proxy specifically breaks RDR2's init (suspected
  module-integrity/anti-tamper reaction to a foreign unsigned dxgi.dll).
- Two real bugs were found and fixed along the way:
  1. `24a54c4` — shared-.rdata vtable patching replaced by per-object
     vtable swap.
  2. `.def` export ordinals did not match system dxgi.dll — ordinal-based
     callers (which RDR2 uses) were landing on the wrong functions. Fixed
     ordinals 1-20 to mirror the system table.
- **The actual fix is configuration**: `sga_vulkan_final_init.*` files and a
  loaded `vulkan-1.dll` prove RDR2 renders via **Vulkan**, not DX12. The
  dxgi proxy is the wrong transport — nothing it hooks ever presents.
  Game config switched `dx12` -> `vulkan`, foreign dxgi/d5c_chain removed
  from the folder, launched through the implicit Vulkan layer.

## PASS via Vulkan layer (in-process d5c_chain)

- Launcher.exe -> RDR2.exe reached the **main menu** (screenshot evidence).
- `%TEMP%\nr_layer_win.log`: 450+ `in-process processed 1920x1080` frames.
- Frame generation cadence healthy: `presents=600 processed=300
  generated=299 native=1`.
- Process stays Running (4.2 GB RSS, window titled "Red Dead Redemption 2").
- Lesson for deploy/launch: a Vulkan-rendered title must not get the dxgi
  proxy; the layer path is mode `vulkan`.
