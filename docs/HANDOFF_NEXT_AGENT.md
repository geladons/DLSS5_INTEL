# HANDOFF — READ THIS FIRST (written 2026-09-22 ~10:15 by Kimi, after owner verdict "not coping")

## Owner's requirement (verbatim, translated)
"The app must work in EVERY mode (video player, windowed mode, games, browser,
whole screen, etc.). We have many references in the project."
Current state per owner 2026-09-22 09:59: demo does not work — clicks FREEZE
everything (kill via Task Manager), screen shows DIGITAL GARBAGE instead of
enhancement, looks terrible. Previous "FIXED + VERIFIED" claims were wrong:
verification was done on bounded synthetic scenarios only (see "How NOT to
verify" below).

## The three independent layers of the problem (do not conflate)

### LAYER 1 — ARCHITECTURE (the killer): fullscreen opaque topmost overlay
m8b-live presents via a FULLSCREEN OPAQUE TOPMOST click-through window.
Consequences, all owner-visible:
- Normal windows open BEHIND the overlay and are INVISIBLE (verified twice:
  Notepad test 22:37 and 09:45; only self-topmost windows like Task Manager
  render above it).
- Every click/drag produces DDA frames -> the event-driven loop runs a full
  51 ms chain PER FRAME -> GPU/queue saturation -> the whole desktop FREEZES.
  (Freeze root cause NOT yet confirmed in detail; hypotheses: FIFO swapchain
  backpressure at 54 ms/frame vs vsync; ACCESS_LOST recovery storms; est-bypass
  flooding. Repro: RUN-DEMO + interact.)
- This CANNOT be patched into working order. It must be re-architected toward
  the reference design (windows-port-plan.md sec.1 + NeuralScreen):
  (a) per-window mode: WGC capture of a TARGET window + an overlay that covers
      only that window's rect (immune to self-capture, other apps unaffected);
  (b) game-attach mode (Vulkan layer / DXGI hook) — optional later;
  (c) fullscreen mode only as an explicit demo/screensaver, not the product.
  The whole-desktop ambition needs a composition-level answer, not a topmost
  window.

### LAYER 2 — INPUT CONTRACT: the network is fed a WRONG input tensor
- features.comp channels 0..2 (deterministic noise) are a STAND-IN integer
  hash. The REAL deterministic_noise() EXISTS at
  work/mlx-dlss/python/mlxdlss/features.py:87 (sinusoid-generated, the file is
  gitignored but present locally). The old comment "cannot be ported" is FALSE.
  Every frame currently feeds the network a wrong conditioning signal.
- Temporal channels 7..9 (previous output) are "first frame repeats 4-6" -
  no real history; the temporal gate (head ch3) is unused. Video/game content
  is processed frame-by-frame with NO temporal state.
- M7/M8a "bit-exact validation" compared the chain against NumPy goldens on
  SYNTHETIC input (seeded sine mix) — it proves the port executes the vendor
  math, NOT that the output is visually correct on real frames. The owner's
  "digital garbage on dynamic content" is consistent with (stand-in noise +
  no history + chaos-bound global blocks) — never visually benchmarked.
=> PREREQUISITE before any quality work: port the real deterministic_noise
   into features.comp (GLSL port of features.py:87), wire real temporal
   history, then LOOK at the output on a real video.

### LAYER 3 — OUTPUT/UX pipeline (mostly fixed, listed for completeness)
- B1 colored lines: FIXED (per-row headpack DC, commit 27d1667). Evidence:
  out_live3 (before) vs out_live4 (after) DDA snapshots.
- B2 clamp-distorted motion: FIXED (adaptive fbcancel clamp).
- Blue-tint accumulation on static content: FIXED (headpack DC + hpfilter +
  resgate) — verified by DDA A/B over 45 s.
- Permanent cursor ghosts: FIXED (cursor no longer composited into the frame).
- Bug #4 enhancement invisible: OPEN — quantified analysis in DEV_STATE
  "Next steps" item 0 (net residual scale 0.05 => ~0.2/255 RMS). Knobs:
  headpack gain, resgate, --strength. Retune AFTER layers 1-2.

## How NOT to verify (learn from the previous agent)
- Do NOT trust "--frames N verify PASS": the gates only check that
  mean|final-native| > 0 and structure > 0 — they PASS ON GARBAGE.
- Do NOT test with a wiggling cursor on a static wallpaper and call it a day.
- Do NOT diff verify BMPs against each other as an accumulation metric —
  "native" is the ECHO capture (our own last frame), and windows ABOVE the
  overlay contaminate everything.
- GDI screenshots CANNOT see the overlay (flip-model). Ground truth = DDA
  (m1dda.exe). Methodology: RUN-DEMO + REAL interactive scenarios (open a
  browser, drag a window, play a video) + m1dda snapshots + OWNER EYEBALLS.

## Inventory / how to run
- App: dlss5/m8b-live (main.cpp ~3500 lines). Build: dlss5\m8b-live\build.cmd
  (auto-falls back to NMake+vcvars — VS COM instance discovery is BROKEN on
  this host, see DEV_STATE watch items). Runner: runm8b.cmd <args> (logs to
  docs\m8b-live.log). Demo: tools\RUN-DEMO.cmd.
- Exe: dlss5\m8b-live\build\Release\m8blive.exe; shaders next to it.
- Pipeline: DDA capture -> CPU bridge -> fbcancel (echo cancel + adaptive
  clamp) -> features (16ch; ch0-2 STAND-IN) -> 71-block chain (47 ms) ->
  headpack (per-row DC) -> hpfilter high-pass -> compose (resgate, strength,
  clamp) -> present via blit to fullscreen overlay.
- Key CLI: --frames N --novideo --refresh-ms 800 --settle-thresh 0.5
  --max-delta 12 --resgate 4 --strength 1.0 --wiggle-idle N --output NAME
  --nocursor --cursor-draw (legacy, paints ghosts) --colorpass 1 (legacy).
- Diagnostics: one-shot [dbg] chain-buffer probe at frame 1 (now includes the
  head4 per-row DC print). Verify frames 10/30/60 dump out\m8b_*.bmp.
- Weights: work/mlxw/dlssnr-logical.safetensors (649 tensors, NOT in git).
- Chain golden/probe ecosystem: dlss5/m8-full-chain + m7-graph-proto.
- Logs: docs\m8b-live.log (all runs), docs\m8-golden.log (chain validation).

## Suggested order for the next agent
1. Kill nothing that matters; read AGENTS.md (host quirks) FIRST — GDI vs DDA,
   detached launches, ASCII-only .cmd, Sunshine is PERMANENT, LUID-based GPU
   pick (LUID changed once already: 95a2 -> 9a8b).
2. Reproduce the owner scenario and SEE the freeze + garbage yourself
   (RUN-DEMO + click around; m1dda snapshots). Root-cause the freeze before
   redesigning anything (perf log: [frame] lines show acq/chain ms; check for
   queue backpressure / ACCESS_LOST loops).
3. LAYER 2 first (cheap, prerequisite): port deterministic_noise from
   work/mlx-dlss/python/mlxdlss/features.py:87 into features.comp; add real
   temporal history (channels 7-9). Then LOOK at a video frame pair.
4. LAYER 1 re-architecture (the big one): WGC per-window mode per
   docs\windows-port-plan.md sec.1 (NeuralScreen's OpenWgc pattern) with an
   overlay limited to the target window rect. This is the owner's core ask.
5. Only then: residual magnitude tuning (bug #4), perf fusion (30 fps path),
   M5 zero-copy backlog.

## What IS solid (don't redo)
M0 coopmat, M1 DDA (incl. idle starvation behavior), M2 interop bit-exact,
M3 front-end ports, M6a weights loader, M7 rounding kernels 100% bit-exact,
M8a chain validated vs goldens (chaos-bound documented), M8c multi-adapter
capture + ACCESS_LOST recovery. The engineering base is real; the product
integration is what failed.
