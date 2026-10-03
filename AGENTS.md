# AGENTS.md - DLSS5_INTEL working rules

> This file is the operating manual for AI coding agents (and automation)
> working in this repository. Humans: start at README.md; CONTRIBUTING.md
> covers the rules that matter for people. The repo is public-facing — never
> commit secrets, weights, binaries or machine-specific paths.

Project: real DLSS 5 (71-block DLSSNR graph) running LIVE on the desktop of an
Intel Arc Pro B50 (Vulkan, DDA capture, click-through overlay). Dev VM
DEV-HOST (Win 11), GPU passed through. Owner plays games via Sunshine
(Moonlight) - Sunshine is PERMANENT, never stop/disable it (PID ~7784).

## Build / run / verify loop
- Build: `cd dlss5\m8b-live && build.cmd` (cmake VS generator, Release; AUTO-
  FALLBACK to NMake+vcvars64 if the VS instance is not discoverable - see the
  VS REGRESSION note below). Check `_build.log` for errors. Exe:
  `dlss5\m8b-live\build\Release\m8blive.exe` (run from that dir - shaders live
  next to the exe).
- Runner: `dlss5\m8b-live\runm8b.cmd <args>` (appends to docs\m8b-live.log).
- Owner demo: `tools\RUN-DEMO.cmd` = `m8blive.exe --frames 1000000` (all defaults).
- Sanity (no overlay): `--novideo --frames 10`. Full video run: `--frames 30`.
  NOTE: --frames counts PROCESSED frames; on an idle desktop the loop parks in
  the settle gate and never reaches the count - keep the cursor moving (external
  ui.ps1 move loop) or pass `--wiggle-idle 1` while testing.
- Verify dumps (only on PROCESSED frames): build\Release\out\m8b_native0.bmp
  (frame 0, clean), m8b_native.bmp / m8b_processed.bmp (last verify frame),
  m8b_frame_{10,30,60}.bmp. Metrics per frame in docs\m8b-live.log.
- Hotkeys while running: CTRL+ALT+Q quit, CTRL+ALT+X toggle overlay (capture-only
  warm mode when hidden; resume re-processes + fbcancel reseed).
- git: `C:\Program Files\Git\cmd\git.exe`. Docs/logs are gitignored - use
  `git add -f` for docs\*.md, *.log, evidence PNGs.

## Host quirks (hard-won, do not rediscover)
- exec wrapper EATS `$vars`/`$_` in inline PowerShell. Write .cmd files (%VAR%
  is immune) or plain -File ps1 scripts. ps1 needs -ExecutionPolicy Bypass.
- NEVER run a long/looping binary foreground (0-token hang). Launch DETACHED
  (`start "" /min cmd /c "exe args > log 2>&1"`), poll the log file.
- ASCII-ONLY in .cmd files and code comments (Russian text once EXECUTED as
  commands - launcher bug 21ba605). Daily notes: English only.
- **GDI SCREENSHOTS CANNOT SEE THE OVERLAY.** It presents via Vulkan flip-model;
  System.Drawing CopyFromScreen shows the plain desktop even with the overlay up
  (docs\_toggle_test1.png proves it). To verify overlay/cursor pixels use DDA:
  `dlss5\m1-frame-capture\build\Release\m1dda.exe frames snap_interval timeout_ms
  outdir wiggle(0/1)` - snapshot_1 is ALWAYS the black warm-up frame; use frames>=3
  and inspect snapshot_2+. ui.ps1 screenshot is fine for normal windows only.
- DDA yields frames ONLY on real desktop updates. Idle desktop = starvation
  ("no DDA frame within ~8s" at startup). First frame of a NEW duplication is a
  BLACK warm-up frame - m8b content-checks and skips it.
- Injected MODIFIERS are dropped on this host (ctrl+alt combos via PostMessage
  fail); SendInput-based input (ui.ps1 move/click, WiggleThread) WORKS.
- Sunshine coexists with our DuplicateOutput (multiple duplications allowed).
- **THE OWNER WATCHES THIS DESKTOP THROUGH THE MOONLIGHT STREAM** (Sunshine
  DDA capture 2560x1440). A WDA_EXCLUDEFROMCAPTURE overlay is INVISIBLE in
  that stream: after echo-free shipped (2026-09-22 ~11:10) the owner saw
  "absolutely nothing" while all readback metrics were green. Owner-facing
  runs MUST pass `--echo-free 0` (fbcancel accumulate path, bounded deltas).
  Conversely, echo-free mode is still right for clean validation readbacks.
- B50 DRIVER BUG (reproduced 2026-10-01): any single compute dispatch whose
  BDA byte-span exceeds ~2 GB (e.g. a batched gemm covering a >2GB tensor)
  returns VK_ERROR_DEVICE_LOST - in-bounds verified, pure driver limit.
  Production never emits such spans; if a future batching change does,
  split the batch.
- Machine: real GPU = Arc Pro B50 (PCI DEV_E212, LUID ...95a2, driver 101.8805,
  Vulkan 1.4.348, coopmat rev2). "Arc Pro B500" in WMI = virtual root display,
  IGNORE for compute. Primary display \\.\DISPLAY5 2560x1440 @ (0,0). QEMU basic
  adapter present. Two DEFAULT_MONITOR (Unknown) = virtual displays (Sunshine +
  QEMU), inactive when no Moonlight client.
- Toolchain: Vulkan SDK 1.4.357.0 (C:\VulkanSDK), CMake 4.4.3, VS Build Tools
  2019 (reinstalled 2026-09-22, instance 8350b0c9, toolset merged from
  BuildTools.bak), Python 3.12.10 (user), Git 2.55. No ninja - VS generator.
- VS REGRESSION (fixed with caveats 2026-09-22): the VS Setup.Configuration
  COM discovery is BROKEN on this host (CLSID/ProgID registration absent from
  every registry view; vswhere returns 0 instances even after a full
  BuildTools reinstall). build.cmd now tries the VS generator (works with the
  existing CMake cache) and falls back to NMake+vcvars64 for a cold configure.
  Backups: BuildTools.bak (orig toolset), _vsbt_backup\ (registrations),
  Downloads\vs_BuildTools.exe (bootstrapper). If builds break again, check
  cmake's first configure line for "instance is not known".

## Architecture map (dlss5\)
- m1-frame-capture: DDA probe/snapshots (diagnostic tool - use it!).
- m2/m3: interop + neural slice. m4-present-simple: CPU-bridge transport (used).
- m5-zerocopy: BACKLOGGED (deadlock on frame-4 handoff; needs validation layers).
- m6/m7/m8: weights loader, graph proto numerics (bit-exact), full 71-block chain.
- m8b-live: THE app (~3400-line main.cpp). Capture (Cap struct, M8c) -> CPU
  bridge -> Vulkan: features -> 71-block chain (~51 ms) -> residual ->
  hpfilter high-pass -> fbcancel (echo subtract + accumulate) -> compose
  (strength, clamp) -> present to fullscreen topmost click-through overlay.
- m11-layer: Windows port of the reference Vulkan present layer
  (nr_layer_win.c, .def exports, VkLayer_dlssnr_win.json). Intercepts
  vkQueuePresentKHR, forces TRANSFER_SRC|DST, executes DLSS 5.
  - x64 games (RDR2, Witcher 3, Cyberpunk): loads `d5c_chain.dll` via C ABI
    and executes directly in-process on host-visible staging buffers (0 ms
    socket lag, no background daemon).
  - x86 games (GTA IV via DXVK): connects over TCP (127.0.0.1:48990) to `m11d.exe`.
  - NO HUD/positional masking (removed Stage 25 - field test showed it covered
    game content and looked worse than no mask). Every pixel is neural-processed;
    the only passthrough is exact-repeat reuse: a frame byte-identical to the
    previous one reuses the last processed result (NR_LAYER_UI_MASK=0 disables;
    legacy env name). Phase30 `auto_mask` is the VENDOR skin/structure
    conditioning parameter (feature ch 12-14), unrelated to HUD masking.
  - Frame generation (`nr_framegen.h/.c`, NrFrameGen): with FG on and the
    in-process x64 engine loaded the layer processes every 2nd present and
    GENERATES the one in between via `dlss5_framegen_transfer` (1:2 cadence,
    no added latency). x86/daemon path: the same 1:2 cadence sends an NRN2
    frame magic to m11d, whose resident engine runs the residual transfer
    (falls back to a full process until a prev pair exists; an old daemon
    that drops the unknown magic is latched FG-off for the session). Watch
    `fg stats: presents/processed/generated/native` in %TEMP%\nr_layer_win.log.
  - Diagnostics: `NR_LAYER_DIAG=1` logs every present (path tag, upload/
    result CRC, transfer status) and stamps an 'NRM' marker into the
    upload buffer so DDA-captured frames can be attributed to the layer.
    `NR_LAYER_CAPTURE` / `NR_LAYER_CAPTURE_OUT` dump raw-in/processed-out.
    KNOWN EXTERNAL DEFECT: a game that SWITCHES THE DISPLAY MODE (GTA IV
    fullscreen 800x600 on a 1440p desktop) lets Windows arbitrate between
    the exclusive surface and a parallel raw surface - ~15% single raw
    frames that bypass every layer path (marker-proven). Fix: native-res
    fullscreen or windowed, both measured clean; m13 launch_args support
    {screen_w}/{screen_h} for this.
- m12-dxgi: DirectX 12 present hook (GTA V Enhanced). Dynamically loads
  `d5c_chain.dll` via C ABI to process present buffers in-process (0 ms TCP lag).
- chain: THE model as an OOP module (`d5c::ChainEngine`). Exported via clean
  C ABI (`chain_c_api.h` / `chain_c_api.cpp` -> `d5c_chain.dll`).
  - Kernel optimizations on Arc B50: GEMM loop unrolling, Q/K/V kernel fusion,
    128-bit vectorized memory ops, epilogue residual add fusion.
  - Bounded contrast-adaptive sharpening (CAS) in `encode.comp` (`pc.c[0]`).
  - Render scale < 1.0 = residual reconstruction: the net runs at netW x netH,
    `compose` emits the SIGNED delta (d.z), the delta is bilinearly upscaled
    and `encode` mode b.w = 2 adds it onto the full-res native. Native detail
    is never replaced by a low-res blow-up. Scale 1.0 path is separate.
  - Deterministic noise seed pinned (`p.b[0] = 1`) to eliminate flickering.
  - Frame generation = residual transfer (`framegen.comp` mode 1,
    `dlss5_framegen_transfer`): generated = cur_raw + warp(prev_processed -
    prev_raw, motion(prev_raw -> cur_raw)). Engine owns prev state
    (`bufPrevFrame_`/`bufPrevRaw_`). Mode 0 (`dlss5_interpolate_frame`) is
    the legacy F0/F1 interpolation, unused by the layer. No XeSS-FG: it needs
    engine motion vectors/depth and is D3D12-only - a present hook has neither.
- m11d: DLSSNR frame daemon for 32-bit x86 titles (TCP 127.0.0.1:48990).
  Lifecycle is auto-managed by `m13`: starts for x86 games, stops after the
  x86 exit grace period, and is reclaimed immediately before an x64 launch.
- m13: DLSS 5 Manager GUI (`m13.pyw`) & In-Game Overlay (`CTRL+ALT+G`).
  - Launch: `launcher.py` GameLauncher runs each launch on its own
    "m13-launch" thread as timed steps (guard_busy, register, resolve,
    guard_running, deploy, daemon, spawn); one `[launch] game=.. step=..
    ok=..` line per step in manager_err.log. Never mutates config; refuses a
    second launch, an already-running game, or a second managed game.
    `worker.py`: ActionWorker queue + WorkerSupervisor watchdog (job > 150 s
    -> fresh worker on the same queue). Deploy never touches sister dirs
    (no bin\x64 <-> bin\x64_dx12 sync).
  - Presets: Native (1.0), Quality (0.67), Balanced (0.58), Performance (0.50),
    Ultra Performance (0.33), and Sharpness (0.0..1.0).
  - Overlay (`overlay.py` + `overlay_win32.py`): Game Bar semantics - topmost
    WS_EX_NOACTIVATE panel that never takes the foreground (Tk takefocus
    stripped; `focus_set` is what steals focus on this host). Photo-Mode
    freeze (`overlay_freeze`) and focus stealing (`overlay_focus`) are
    OPT-IN, default off. Cursor release (`AttachThreadInput` +
    `ReleaseCapture`) on open; log `[overlay] shown ... fg_before/fg_after`.
- Weights: work\mlxw\dlssnr-logical.safetensors (proprietary, NOT in git;
  649 tensors, 71 blocks, resident ~325 MB VRAM). Loader m6.

## Code rules (owner directive 2026-09-22 - mandatory, always)
Large files already make edits painful (main.cpp ~3900 lines). From now on:
- OOP/modular design is MANDATORY for new code: classes with a single clear
  responsibility, encapsulated state, explicit ownership. No new file-scope
  globals; state travels in structs/classes passed by reference.
- NEVER grow main.cpp for a new concern. New subsystem = its own translation
  unit (foo.h/foo.cpp) with a small public interface, wired in at one call
  site. Naming style: ChainRecorder, CaptureSource, OverlayPresenter.
- File-size discipline: soft cap ~600 lines per file; when a file grows past
  it, split by responsibility before adding more.
- When touching code inside an oversized file, prefer EXTRACTING the touched
  piece into a module over editing in place - but only when the extraction is
  safe and covered by the validation rig (below). No heroic untested rewrites.
- One logical change per commit, small reviewable diffs. Anything touching the
  chain or shaders MUST be rebuilt and validated (features/head dumps vs torch
  goldens + 45-frame PASS run) before commit.
- Comments and identifiers: English, ASCII-only (host quirk above).

## Red lines
- Don't stop Sunshine; don't kill openclaw/gateway processes (Kimi desktop owns
  them - quit Kimi tray app instead if truly stuck).
- Weights and reference\ binaries stay out of git.
- Don't commit real credentials anywhere.

## Current status -> see DEV_STATE.md
