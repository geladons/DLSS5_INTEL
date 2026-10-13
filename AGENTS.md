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
    Serving is SYNC by default (present blocks on the daemon round-trip,
    ~15-18 fps engine cadence, every displayed frame real): bare async
    replayed whole stale frames = ghosts/flicker on motion (owner field
    report 2026-10-08, unmanaged launch env). Explicit NR_LAYER_ASYNC=1
    opts back into the mailbox path - only worth it with
    NR_LAYER_RESIDUAL=1 + NR_LAYER_GPUWARP=1 armed (the masked-delta
    serve); autotest launches set ASYNC=1 directly. m13 gamelaunch pins
    ASYNC=0/RESIDUAL=1/GPUWARP=1 for every arch=x86 game - do not launch
    x86 test sessions with hand envs that miss this block.
  - Small-delta patch serve (NR_LAYER_PATCH, OPT-IN, default OFF, sync
    path only): input differing from the last ANSWERED frame only inside
    a cursor-scale box (<=2% px / <=8% bbox, env-tunable) is served
    instantly as held processed result + raw bbox region - the menu
    cursor runs at full present rate instead of the ~15 fps daemon
    cadence (owner lag complaint). Anchor (`earlier`) is NOT advanced;
    trail accumulates until bounds or NR_LAYER_PATCH_CAP (48) force a
    resync exchange. diag tag `p=patch`. REJECTED 2026-10-09 as a
    default: the raw bbox is visibly unprocessed next to the enhanced
    frame (a raw rectangle trails the cursor). Keep OFF; the cursor
    stays at daemon cadence.
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
    temporal_nr then runs a RESIDUAL-DOMAIN hold on the emitted value
    (v9, 2026-10-10; replaces the output-domain HOLD/RELEASE latch).
    NEVER hold the OUTPUT value again: a latch that re-emits the stored
    byte while dist(cand, ref) < deadband ignores whether the INPUT
    under the pixel changed, so moved/faded content leaves a permanent
    sub-threshold ghost (proven 2026-10-10, gate
    `replay_gate.py fixture:ghost` on the old build: slide residue
    4.4%/max 23, fade 7.1%/max 20 of black-input pixels).
    bufHist (binding 13, fp32[TOK*10], N = pc.a.x) layout:
      [0,3N)   E = last emitted byte per channel (scaled_color store,
                    unchanged; features reads it as history)
      [3N,6N)  P = previous frame's input col01 (rewritten every frame
                    EXCEPT pinned frames - see the E_HOLD deadband below)
      [6N,9N)  A = drift anchor input (col01 at the last fresh emit /
                    start of the current refresh burst)
      [9N,10N) R = remaining refresh frames (0..R_FRAMES)
    Held residual rh = Eb - P (previous emitted byte minus the input it
    was produced on); fresh residual rc = cand - col01 (pre-anchor);
    residual mismatch dr via the same chroma-aware metric
    (max(BT.601 luma dist, 2.5 * max channel dist)). FRESH emit
    (uncapped gain-anchored candidate, A=col01, R=0) on:
    uninitialised/out-of-range state, reset frame (pc.d.x==0),
    per-frame input jump dF = |col01-P| > IN_CUT=24/255, or
    dr >= DB_CUT=128/255. Otherwise emit = col01 + rh +
    clamp(rt - rh, +-capc) where rt = anchored - col01 and capc is 0
    outside a refresh burst: static input (col01 == P) reproduces the
    previous emit byte-exact - the same fixed point that pins the
    net's gain>1 period-2 limit cycle on its own output as history
    (ANY proportional deadband/trickle rides the chase forever -
    measured ~24-42% movers on identical frames; do not soften).
    Slowly changing input -> output tracks input 1:1 while the held
    residual rides along; no stale output ghost can survive an input
    change. Residual REFRESH is anchor-armed and budgeted: when
    |col01-A| > D_REF=12/255 (distance from a FIXED anchor - 1-2 LSB
    input noise cannot integrate toward it; an accumulating drift
    counter was tried first and refreshed forever on grain) the anchor
    re-bases and arms R_FRAMES=16 frames of PROPORTIONAL steps
    capc = max(R_STEP_MIN=1/255, R_FRAC=0.25*|rt-rh|) per channel -
    exponential convergence, ~1/255/frame on small mismatches (below
    the x1.69 compose gain*intensity amplification that made fixed
    4/255 steps visible as pops) while ghost-class errors still close
    in ~10 frames; the burst ends early once dr < R_DONE=6/255 and is
    bounded by construction. fv[15]==0 pixels run the identical hold -
    no gate special-casing (the 2026-10-07 static-shimmer rationale is
    unchanged: the gate compares processed emit vs raw input so still
    pixels with a >band residual read "moved" forever; unlatched they
    re-emitted the raw jittering prediction - ~47% of a GTA IV menu
    frame, 95%+ re-emitting a new byte/frame). The emitted value is
    byte-snapped and stored; the identical rule must stay mirrored in
    temporal_stats.comp (readonly view of the same pre-dispatch
    E/P/A/R) or the gain anchor chases a ghost.
    EMIT DEADBAND + P-FREEZE (v14, 2026-10-12): byte-exact static
    input hits the col01==P fixed point, but real captures are never
    byte-exact - DXVK/codec dither jitters the input +-1 LSB, which at
    net res becomes sub-LSB col01 noise. The emit then re-derives E
    through the byte snap every frame; each +-1 LSB correction near
    the rounding boundary integrates into E permanently while P
    advancing under the jitter lets rh = E - P erode - together an
    UNBOUNDED random walk that grows visible garbage on static scenes
    (field report, GTA IV: litter accrues second by second while
    still, cleans where content moves; reproduced on a 1244-frame
    still segment of a real _live capture - hf energy 10 -> 30 over
    300 frames, stored E drifting ~6 LSB/90 frames vs 0.5 LSB input
    jitter). Fix, held path only: pinned = (!refresh && dA <=
    E_HOLD=1.5/255) where dA is drift from the FIXED anchor A (NOT
    the per-frame increment |em-Ew| - a monotonic sub-band drift would
    stay pinned forever under that gate while rh absorbs the error);
    on pinned frames emit = Ew verbatim AND P carries aP forward
    instead of col01 so rh stays invariant - jitter can never enter
    the residual bookkeeping. A real move accumulates past the band,
    re-emits col01 + rh (input + frozen residual - nothing was eaten),
    and dA > D_REF still arms a refresh as before. Two alternatives
    measured and rejected: anchor-referenced emit col01 + (Ew - aA)
    (emits a STALE residual when the anchor lags - fixture:ghost max
    28 vs shipped 13) and pinning without freezing P (jitter still
    erodes rh - walk continues at ~40% rate). Validated: dithered-
    static hf flat 16.8 -> 17.1, E drift 0.096 LSB/90f, 96.5% pinned;
    fixture:ghost identical to shipped (slide 0.06%/max 13).
    The hold is MOTION-COMPENSATED (v10/z1, 2026-10-10): an unwarped
    residual hold drags enhancement detail in screen space on pans
    (v9 measured: screen-fixed residual energy 16.4% vs the 7.5%
    temporal-off floor; flow flicker on a real flythrough equal to
    temporal-off ~20%) - temporal state belongs to the CONTENT, not
    the screen cell. recordTemporal order each frame: vkCmdCopyBuffer
    bufHist -> bufHistPrev (binding 17, same TOK*10 layout) +
    transfer->compute barrier, then motion_est, temporal_stats,
    temporal_nr. Prior state (E/P/A/R) is ALWAYS read from hsPrev -
    the warped neighbour taps would otherwise race temporal_nr's own
    in-place write to binding 13. motion_est.comp: one workgroup per
    8x8 net-res block, luma of current col01 vs hsPrev's P, integer
    SAD full search over (dx,dy) in +-12, ZERO-MOTION BIAS picks (0,0)
    when SAD00 <= SADmin*(1+D5C_MV_ZBIAS=0.15) + 64*D5C_MV_ZABS=2/255
    (without it 1-2 LSB noise parks a random +-1 vector on static
    blocks and warped pixels then flicker - the z0/b15i1 static-fail),
    parabolic subpixel refine on ALL interior picks including (0,0) -
    a bias-pinned sub-pixel drift still shows a SAD slope (Stage 60
    slow-zoom judder fix: on z1 a 1.003x/frame zoom pinned ~92-94% of
    blocks at (0,0), left the residual screen-anchored and flipped the
    rest to the moving path - the discrete flip was the shake). For a
    (0,0) pick the refined fraction must beat D5C_MV_IMPT=0.06
    (pc.d[1]): imp = sum_axes(den*delta^2/2), the predicted SAD drop at
    the refined offset, scales with local gradient so detailed content
    with real drift passes and SAD-flat blocks (flat sky, codec grain -
    meaningless +-0.5-rail fits) collapse to exact (0,0); pinning flat
    content is visually free. D5C_MV_SUBT=0.10 (pc.c[3]) snaps the
    final sub-1 vector under the noise floor to 0. D5C_MV_EMA (pc.c[0],
    weight of new sample, default 1.0 = RAW/off) is an optional
    per-block temporal filter with a >=1px snap and direction-reversal
    restart - measured worse at 0.5 (_pan_seq alternation at the snap
    boundary), kept only as a knob. bufMv (binding 18)
    = vec4(mvx, mvy, conf, -) per block. temporal_nr/stats fetch the
    per-pixel mv NEAREST-BLOCK or, with D5C_MV_INTERP=1 (shipped),
    bilinear between the four nearest block centres (8i+3.5). State
    reads warp to q = p - mv: out-of-image -> fresh; integer mv ->
    verbatim source pixel; fractional -> bilinear P, A and the
    byte-DECODED E (no re-snap), R from the nearest tap; all taps
    validated. moving = |mv| > 0.01 -> refresh every frame at
    D5C_MV_BETA=0.15 scaled by min(|mv|,1) (continuous static->moving
    boundary, no discrete path flip) with no budget spent; !moving ->
    the exact v9 anchor/budget path (static output stays
    byte-identical).
    D5C_MV_PIXSTATIC (default 0) can force mv=(0,0) per pixel when the
    unwarped input delta is under N LSB - measured useless at 1 LSB,
    kept off. D5C_DEBUG_MV=1 logs median mv + nonzero share /30 frames.
    MOTION GATE (host-side, engine.cpp detectSceneCut): the same 64x36
    luma thumb that detects cuts also counts moved cells (>10/255);
    fraction >TMGATE_HI (0.20) -> histSuppressed_ -> the frame records
    histValid=2 everywhere (features d[0], temporal_stats, temporal_nr).
    Mode 2 is now SHADER-EQUIVALENT to mode 1 (v9 residual hold,
    2026-10-10): temporal_nr/temporal_stats run the identical rule on
    both modes - the pan/drift that tripped the gate moves every
    pixel's input past IN_CUT, so emits go fresh and no held residual
    can freeze a stale mask, and a mode switch lands without a
    transition flash (the field's "picture literally pulses" was a
    latch->emit jump: measured +6.6 global luma in ONE frame @gate
    re-engage on the old output-hold build). Suppression must NOT drop
    to 0: pure raw-prediction frames shimmer every pixel each frame and
    on sustained pans the gate would hold that mode for the whole pan
    (out mad med 1.89 vs 0.33 unsuppressed, real GTA IV flythrough
    replay). <TMGATE_LO (0.05) -> re-engage,
    hysteresis holds between. History keeps refreshing on suppressed
    frames, so re-engage is transient-free. Envs
    *_TMGATE=0 off, *_TMGATE_HI/LO retune, D5C_DEBUG_TMGATE=1 logs.
    DRIFT GATE (same detectSceneCut pass, 2026-10-09): per-frame velocity
    is blind to slow pans - a ~0.25 px/frame turn keeps every coarse cell
    under the 10/255 delta so tmFrac ~ 0 and the gate never sees motion,
    while the per-pixel emit latch keeps re-emitting the frozen mask on
    content that slid under it ("scene holds ~1 s then slowly turns ->
    mask applied crooked"). driftThumb_ snapshots the thumb at the last
    SETTLED pose; driftFrac_ = fraction of cells displaced >DRIFT_DELTA
    (5/255) from it on the 3x3-box-smoothed thumb (box filtering halves
    noise sigma: sigma=6 grain = 1% displaced, a 1 px pan = 10%).
    driftFrac > DRIFT_HI (0.20) suppresses identically; while suppressed
    the anchor stays FROZEN so a continuing pan keeps driftFrac rising -
    release needs near-zero coarse velocity (tmFrac < DRIFT_RELLO 0.005;
    even a slow pan still moves a few coarse cells) AND driftFrac
    plateaued (<DRIFT_EPS 0.008) for DRIFT_CALM (8) frames, then the
    anchor re-bases at the new pose. While live, driftFrac < DRIFT_LO
    (0.05) for DRIFT_SETTLE (10) frames re-bases the anchor so the
    measure stays a displacement from a real pose. Measured on the
    freeze->0.25 px/frame pan probe: suppression fires ~15 frames in,
    holds for the whole pan (stale-byte fraction ~4% -> ~0.8%), releases
    ~8 frames after the pan stops; grain-only content never trips it.
    Envs *_TMGATE_DRIFT=0 off, _DRIFT_HI/LO/EPS/CALM/SETTLE/DELTA/RELLO.
    D5C_CUT_LIMIT=0 disables the whole thumb pass incl. the gate.
    Fixes the global-receptive-field "breathing" (cursor move re-derives
    all tones) at the x86 daemon cadence without re-importing the
    Stage-45 stale-structure noise. Measured: jitter breathing 0.11 vs
    2.47/255 forced-off; pan suppresses (39.9% moved -> history off).
    RELEASE GATE for all of the above: tools/autotest/replay_gate.py
    feeds a raw BGRA sequence to a running m11d over the TCP protocol
    and scores conditioned metrics screen capture cannot see -
    transition-flash (one-frame global luma step the input did not
    make), still-input flicker (info line: >4/255 emit changes on
    input-still pixels), STATIC flicker (same >4/255 changes but only
    pixels >16px from ANY input motion; FAIL at p90 >2%), FLOW flicker
    (output vs the motion-warped previous output, evaluated where the
    input's own optical flow is exact; FAIL at med >10% or p90 >20%,
    WARN at med >4% / p90 >8%), frozen-stale (emit pinned while input
    moved), invented luma oscillation, and `fixture:ghost` (synthetic
    slide + fade-out text: FAILs when residue >6/255 remains on
    black-input pixels - the output-domain-hold ghost class).
    Every temporal change must pass:
      python tools/autotest/replay_gate.py fixture:ghost
      python tools/autotest/replay_gate.py D:\dlss5_cap\_pan_seq.raw
        (341-frame real GTA IV benchmark flythrough)
      python tools/autotest/replay_gate.py Temp\_pan_man.txt
        (synthetic integer pans +3,0 / +1,1 / -6,0 of a real GTA IV
        frame, manifest machine-local)
    and be checked on:
      python tools/autotest/replay_gate.py D:\dlss5_cap\_nat_load.raw
        (native GTA IV loading screen + flythrough start; built from
        at_s52_benchnat via Temp\_nat_extract.py - machine-local
        capture, not committed)
    The 2026-10-09 "picture pulses" field build FAILs (amplified f65
    +5.6, still-flicker p90 10%/max 62%); the shipped z1 motion-
    compensated build passes fixture:ghost + _pan_seq clean and
    _pan_man/_nat_load at WARN (static p90 ~1-1.5%, flow ~1%/6.4% med).
    Do NOT ship temporal changes without running it - owner requirement.
    Capture harness quirks (2026-10-10): GTA IV's window now launches
    MAXIMIZED even with -windowed - capture_session.py needs
    `--park 872,400,816,638` to pin the game window onto the record
    region; and analyze.py takes a VIDEO FILE source - pass
    <outdir>\capture.mp4 explicitly (a bare dir is treated as a PNG
    frame dir and stray *.png in the outdir get picked up).
  - Input contract = 16 feature channels exactly (input_adapter_weight
    [16,32]): ch 0-2 noise, 3 const, 4-6 scaled colour, 7-9 history,
    10-14 Phase30 controls, 15 zero. NO motion-vector/depth slots - the
    model is an enhancer, not an engine-integrated temporal upscaler.
  - Gain anchor (binding 16, `temporal_stats.comp`): features accumulates
    the input luma sum, temporal_stats the pre-anchor output sum, and
    temporal_nr rescales the per-pixel residual so the frame's mean
    residual follows a SLEW-LIMITED EMA (`DLSS5_GAIN_LOCK`/
    `NR_LAYER_GAIN_LOCK`/`D5C_GAIN_LOCK`, default 0.08 = ON; env =0
    disables). stats[6] tracks the residual DC the emit may APPLY;
    it chases the net's own dCur at <= ~1.2/255 mean per frame (SLEW=3
    Q8) - the old hard snap re-applied a clamped residual in one frame
    (a sub-threshold input shift that flipped the net's tone decision
    landed as a +6.6/255 whole-frame brightness pop). k = ema/dCur
    clamped to 0..1: growing residual becomes a ~5-frame ramp, shrinking
    residual applies immediately (never hold stale enhancement), a
    polarity flip emits neutral input until ema crosses. histValid=0
    frames reseed ema = dCur so a new scene is never clamped by the old
    scene's track. Deadzoned (|dCur-ema| < 6% -> k=1). m8b-live binds
    the stats buffer for shared features.comp but never reads it;
    missing temporal_stats.spv = graceful gain-lock off. The EMA is
    written to spare slot [7] and rotated into [6] by temporal_stats
    next frame - never write [6] from temporal_nr (intra-dispatch race).
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

## GitHub mirror (public) - sync procedure
Canonical remote is gitea (master). PUSH TO GITHUB ONLY ON EXPLICIT OWNER
REQUEST - routine work goes to gitea constantly, the mirror syncs when the
owner asks. GitHub
(https://github.com/geladons/DLSS5_INTEL.git) carries a SANITIZED rewritten
history (filter-repo, 2026-10): local-only notes/, Temp/, build*/ dirs,
*.exp/*.lib/*.obj and machine paths (Users\<owner>, DEV-HOST) are stripped
from every commit, so GitHub SHAs intentionally differ from gitea. To sync
new work: fresh `git clone --no-hardlinks` into a staging dir, re-run
filter-repo with the same rules (see below), `git push --force github
master:master`. Never push the raw local history - it contains local-only
files and machine paths. Incremental sync WITHOUT force-push (preferred,
verified 2026-10-12): in the staging clone fetch gitea+github, hard-reset to
gitea/master, re-run the filter, then `git rebase --onto github/master
<filtered-base> <filtered-head>` and verify `rev-parse <picked>^{tree} ==
<filtered>^{tree}` for every commit before pushing fast-forward. NOTE: the
filter also rewrites THIS section's literal rules text on the GitHub side
(that is the point) - expect that diff. Full-rewrite fallback:
`git push --force github master:master`. Replacement rules file (repl.txt):
  Users\<owner>==>Users\<owner> ; Users/<owner>==>Users/<owner> ; Users\\AI==>Users\\<owner>
  DEV-HOST==>DEV-HOST ; D:\games==>D:\games ; D:/games==>D:/games
  invert-paths: --path notes/ --path Temp/ --path-regex ".*[/]build[^/]*[/]|^build[^/]*[/]"
  --path-glob "*.exp" --path-glob "*.lib" --path-glob "*.obj" --path dlss5/m13/_ui_shot.png
  --message-callback 'return message.replace(b"Users\\AI", b"Users\\<owner>").replace(b"Users/<owner>", b"Users/<owner>").replace(b"DEV-HOST", b"DEV-HOST").replace(b"downdloads", b"games")'

## Current status -> see DEV_STATE.md
