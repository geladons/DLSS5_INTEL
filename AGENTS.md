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
  fail); SendInput-based input (ui.ps1 move/click, WiggleThread) WORKS for
  desktop windows. GAMES are different: GTA IV (DInput/raw-input) ignores
  ALL injected input - mouse_event, SendInput mouse+key, PostMessage
  WM_LBUTTONDOWN, MCP Click, UIA Invoke all no-op (verified 2026-10-03);
  SetCursorPos DOES move the OS cursor and PostMessage BM_CLICK DOES click
  native dialog buttons. BUT the ViGEmBus virtual X360 pad WORKS end-to-end
  (verified 2026-10-05: vgamepad Python module, drives menus + gameplay) -
  gamepad input is THE synthetic route for menu/gameplay automation, see
  tools/autotest/pad.py. Keyboard/mouse progression still needs Sunshine's
  virtual HID (owner's Moonlight session) or a physical device.
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
- Machine: real GPU = Arc Pro B50 (PCI DEV_E212, LUID ...95a2, driver 101.8976
  as of 2026-09-28 - TDRs still reproduce on it; do not assume a driver
  update fixed BMG coopmat/WDDM faults),
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
- m14-ddarec: CFR DDA streamer (ddarec.exe secs fps x,y,w,h -) - pipes
  rawvideo BGRA to stdout for ffmpeg; repeats last frame on timeout so
  VFR games land as clean doubles. Pair with tools/gamerec/gamerec.py
  (record --window <exe> + analyze -> per-frame luma/flicker CSV).
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
  - Frame generation (`nr_framegen.h/.c`, NrFrameGen): two algorithms.
    Mode 2 (DEFAULT for opt-in FG, `*_FG_MODE=2`, LS-class): the layer keeps
    a 1:2 cadence - on the mid present it captures the REAL middle frame
    into `data->mid` and re-blits the held processed anchor; on the anchor
    present it calls `dlss5_framegen_interpolate` (x64 in-process) or sends
    the mid payload as an NRN3 magic to m11d (x86) - the generated midpoint
    of the last two processed anchors goes out at a uniform +1-present
    delay. Mode 1 (`dlss5_framegen_transfer`, NRN2) is the legacy +0-latency
    residual transfer, kept for latency-critical games. Watch
    `fg stats: presents/processed/generated/native` + `diag_path=fgi/mheld`
    in %TEMP%\nr_layer_win.log.
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
    2026-10-08 UPDATE: the same defect fires in 800x600 WINDOWED mode on
    the game-select screen specifically. Proven exhaustively: 6000
    consecutive presents upload byte-identical processed content (constant
    up= CRC, dense f=, zero bypassed/native presents), yet ~every 0.5-3 s
    a 1-2-frame raw flash splices the whole CLIENT area only (borders
    untouched), carrying NO NRMK marker and tracking the LIVE scene. Not
    the redirection bitmap (DC polls show processed 114/114), not IFlip
    (topmost full-cover overlay no-op), not WS_EX_LAYERED/TOPMOST, not
    device resets (none during captures), not trigger/_access flicker
    (file never missing). Flashes are ABSENT on the loading screen and in
    gameplay (marker scan clean over 900+ frames) - the defect is bound to
    the menu-select presentation mode. No user-mode fix found; only the
    native-res/fullscreen path avoids the arbitration. Autotest labels
    these "external-raw-splice" (byte-exact replay of an older frame:
    lag2_resid ~0, NRMK absent when it survives the codec).
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
  - Temporal path (`temporal_nr.comp`): the vendor blend outp = pred +
    alpha*(hist01 - pred) runs in-place on the head before compose; the
    next history is the PRE-Phase30 output. features.comp sanitises the
    history INPUT (ch 7-9) with a TAA-style AABB clamp: hist may only
    reach colours in the current 3x3 neighbourhood (+/-0.004 scaled) -
    kills cursor trails, stale-region drag, and the bright-patch
    relaxation oscillator a scalar reject gate produced (field report
    2026-10-03). Do NOT reintroduce a scalar alpha gate - it oscillates.
    temporal_nr additionally gates alpha PER PIXEL (hold_floor port):
    alpha *= fv[15] - a factor features.comp computes on the UNCLAMPED
    history vs the current colour, max(BT601 luma dist, 2.5 * max channel
    dist) through the same soft 1-smoothstep(0.02,0.10) band - still
    pixels keep the pull, moved pixels and isoluminant chroma drifts
    reconstruct from the net alone. fv[15] is 0 on history-reset frames
    (features' histValid), collapsing the blend to the full-strength
    prediction there. x16 ch7-9 are additionally mixed toward the
    first-frame layout by the same distance (fv keeps the clamped copy
    for the blend). temporal_stats.comp mirrors the same gate so
    stats[1] matches the emitted output.
    temporal_nr then runs a HOLD/RELEASE latch on the emitted value
    (5d238f6, flicker fix): if the pre-anchor blend candidate is within
    DB_HOLD=32/255 of current history (same chroma-aware metric), emit =
    history verbatim - the net's response to its own output fed back as
    history has gain>1 (period-2 limit cycle, +-4-5 LSB), so ANY
    proportional deadband/trickle rides the chase forever; only a hard
    hold pins the loop. Do not soften it into a smoothstep/k_min form -
    that reopens the chase (measured: ~24-42% movers on identical
    frames). The emitted value is byte-snapped and stored; the identical
    step cap must stay mirrored in temporal_stats.comp or the gain
    anchor chases a ghost.
    RELEASES ARE BANDED + RATE-LIMITED (release-pop fix): a candidate AT
    the threshold used to flip hold->release->hold and emit the full
    ~32+/255 candidate every flip (fieldfix.mp4: chroma/luma pop
    clusters on a static loading screen). Now the emit cap is chosen by
    the distance band: d<32/255 hold, [32,64)->STEP_LO=8/255,
    [64,128)->STEP_HI=24/255 per channel, >=128/255 uncapped (real
    change/scene cut lands in one frame). The stored history IS the
    Schmitt margin: each release-step moves hist STEP closer to cand,
    so a flipped attractor lands STEP<DB_HOLD away -> re-HOLDS; for
    gaps <64/255 the ratchet is monotonic and provably terminates
    (unit-proven: +-35/255 alternation collapses to one 8/255 micro-
    step then pins). >64/255 persistent alternation can still shuttle,
    bounded at STEP_HI/frame vs the old full-gap pops.
    fv[15]==0 pixels are LATCHED TOO (2026-10-07 static-shimmer fix):
    the gate compares the previous PROCESSED emit to the RAW input, so
    still pixels whose enhancement residual exceeds the band stayed
    gate=0 forever and re-emitted the raw unblended prediction every
    frame - the measured 1-6 LSB wander lived entirely there (~47% of a
    GTA IV menu frame, 95%+ re-emitting a new byte/frame; gated pixels
    were pinned to <1%). On non-reset frames (pc.d.x=1) gate=0 pixels
    now run the same banded latch against prevB = their own previous
    EMITTED BYTE (the raw stored history, never the box-clamped h01 -
    a legit emit may sit outside the 3x3 box and an h01 hold would
    ratchet into a 2-cycle). Static pred jitter << DB_HOLD pins; real
    motion lands >DB_CUT and emits instantly; only the history-reset
    frame bypasses (stale bufHist must not anchor). Same bands and
    reference rule must stay mirrored in temporal_stats.comp.
    MOTION GATE (host-side, engine.cpp detectSceneCut): the same 64x36
    luma thumb that detects cuts also counts moved cells (>10/255);
    fraction >TMGATE_HI (0.20) -> histSuppressed_ -> the frame records
    histValid=0 everywhere (features d[0], temporal_stats, temporal_nr)
    = pure prediction, temporal-off behaviour on fast content where the
    ~40 ms cadence would drag stale structure; <TMGATE_LO (0.05) ->
    re-engage, hysteresis holds between. History keeps refreshing on
    suppressed frames, so re-engage is transient-free. Envs
    *_TMGATE=0 off, *_TMGATE_HI/LO retune, D5C_DEBUG_TMGATE=1 logs.
    D5C_CUT_LIMIT=0 disables the whole thumb pass incl. the gate.
    Fixes the global-receptive-field "breathing" (cursor move re-derives
    all tones) at the x86 daemon cadence without re-importing the
    Stage-45 stale-structure noise. Measured: jitter breathing 0.11 vs
    2.47/255 forced-off; pan suppresses (39.9% moved -> history off).
  - Input contract = 16 feature channels exactly (input_adapter_weight
    [16,32]): ch 0-2 noise, 3 const, 4-6 scaled colour, 7-9 history,
    10-14 Phase30 controls, 15 zero. NO motion-vector/depth slots - the
    model is an enhancer, not an engine-integrated temporal upscaler.
  - Gain anchor (binding 16, `temporal_stats.comp`): features accumulates
    the input luma sum, temporal_stats the pre-anchor output sum, and
    temporal_nr rescales the per-pixel residual so the frame's mean
    residual follows an EMA (`DLSS5_GAIN_LOCK`/`NR_LAYER_GAIN_LOCK`/
    `D5C_GAIN_LOCK`, default 0.08 = ON - bounded anchor; env =0
    disables). Kills DC breathing on pans;
    >50% residual jumps snap through (scene changes). Per-frame
    correction k is DEADZONED (|dCur-ema| < 6% -> k=1) and clamped to
    0.80..1.25 - never widen the clamp, a wider range re-introduces
    the strength whipsaw fixed 2026-10-04. m8b-live binds the
    stats buffer for shared features.comp but never reads it; missing
    temporal_stats.spv = graceful gain-lock off. The EMA is written to
    spare slot [7] and rotated into [6] by temporal_stats next frame -
    never write [6] from temporal_nr (intra-dispatch race).
  - Frame generation (engine_framegen.cpp): mode 1 = residual transfer
    (`framegen.comp`, `dlss5_framegen_transfer`): generated = cur_raw +
    warp(prev_processed - prev_raw, motion(prev_raw -> cur_raw)). Mode 2 =
    anchor interpolation (`fginterp.comp`, `dlss5_framegen_interpolate`):
    symmetric rawA<->rawB match, warps BOTH processed anchors to factor,
    per-pixel confidence falls back to the real captured midRaw - honest
    output on scene cuts and disocclusion. Engine owns the two-anchor
    state (bufPrevFrame_/bufCurProc_ proc pair, bufPrevRaw_/bufCurrFrame_
    raw pair, bufMidRaw_) rotated inside processFrame; `set_framegen_mode`
    (env DLSS5_FG_MODE/NR_LAYER_FG_MODE/M12_FG_MODE) selects the rotation,
    mode 2 clamps to 1 when fginterp.spv is absent. Mode 0
    (`dlss5_interpolate_frame`) is the legacy F0/F1 interpolation, unused
    by the layer. No XeSS-FG: it needs engine motion vectors/depth and is
    D3D12-only - a present hook has neither.
- m11d: DLSSNR frame daemon for 32-bit x86 titles (TCP 127.0.0.1:48990).
  Lifecycle is auto-managed by `m13`: starts for x86 games, stops after the
  x86 exit grace period, and is reclaimed immediately before an x64 launch.
  m13 monitor auto-restarts a dead daemon while an x86 game lives (bounded:
  5 tries, one per 30 s). m11d self-bounds engine loss: kDeadEngineLimit=3
  consecutive dead engines or failed builds without a served frame between
  -> exit(4). Never widen it - an unbounded rebuild loop after a TDR
  (VK_ERROR_DEVICE_LOST) starves the game with back-to-back ~3.5 s/676 MB
  builds (2026-10-07: two BMG_HWS_VULKANCOMPUTE_PAGEFAULT resets froze the
  session; the storm, not the first fault, is what hung everything).
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
