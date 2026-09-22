# AGENTS.md - DLSS5_INTEL working rules

Project: real DLSS 5 (71-block DLSSNR graph) running LIVE on the desktop of an
Intel Arc Pro B50 (Vulkan, DDA capture, click-through overlay). Dev VM
DESKTOP-285INKS (Win 11), GPU passed through. Owner plays games via Sunshine
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
- Weights: work\mlxw\dlssnr-logical.safetensors (proprietary, NOT in git;
  649 tensors, 71 blocks, resident ~325 MB VRAM). Loader m6.

## Red lines
- Don't stop Sunshine; don't kill openclaw/gateway processes (Kimi desktop owns
  them - quit Kimi tray app instead if truly stuck).
- Weights and reference\ binaries stay out of git.
- Don't commit real credentials anywhere.

## Current status -> see DEV_STATE.md
