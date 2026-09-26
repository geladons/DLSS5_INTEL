# Architecture — how DLSS5_INTEL works

This document describes the demo build (M13 v6, 2026-09-25). It is written for
engineers who want to understand — or contribute to — the pipeline.

## High-level picture

```
                        ┌────────────────────────────────────────────┐
                        │                 M13 Manager                │
                        │  (Python 3.10+, Tkinter, stdlib only)      │
                        │  scan games · deploy DLLs · launch · knobs │
                        └───────┬───────────────────────┬────────────┘
                                │ ENABLE_NR_LAYER=1     │ knob files / NRCT
                                │                       │ (TCP 47990, masked magic)
   ┌────────────────────────────┼───────────┐   ┌───────┴────────┐
   │ game, Vulkan     │ game, DX9-11        │   │ game, DX12     │   screen / window mode
   │ (native)         │ (DXVK deployed)     │   │ (dxgi proxy)   │
   └──────┬───────────┴──────────┬──────────┘   └───────┬────────┘
          │ vkQueuePresentKHR    │ vkQueuePresentKHR     │ IDXGISwapChain::Present
   ┌──────┴──────────────────────┴───────────────────────┴────────┐
   │  frame grab → 16-byte header {magic,w,h,format} + BGRA        │
   │  TCP 127.0.0.1:47990 → m11d daemon                            │
   └──────────────────────────────┬───────────────────────────────┘
                                  ▼
   decode → features → featpack → 71-block DLSSNR chain → head
   → gather residual → high-pass filter → feedback echo-cancel accumulate
   → compose (out = clamp(src + blend·(pred − src)) · gain) → encode → readback
                                  │
                                  ▼ BGRA back over TCP / shared bridge
   into the swapchain (game modes)  ·  fullscreen Vulkan overlay (screen mode)
```

## Components

### `dlss5/chain` — the model as a module

OOP module extracted from the live app (verbatim numerics):

- `VkContext` — Vulkan device init, cooperative-matrix capability checks
- `WeightsStore` — loads the Safetensors weights, packing / fuse-fold /
  de-swizzle into the resident layout
- `ChainArena` — slot layout, 3.5 GiB chunking, buffer device addresses
- `ChainRecorder` — 14 pipelines, walks the 71-block U-Net graph
- `ChainEngine` — the echo-free front-end: decode → features → featpack →
  chain → compose → encode → readback

Shaders compile from `dlss5/m8b-live/shaders` (single source of truth for all
consumers). Consumers: `m11d` (game modes) and `m8b-live` (screen mode).

### `dlss5/m11-layer` — Vulkan implicit layer

Windows port of the reference Vulkan present layer (`nr_layer_win.c`, `.def`
exports, manifest `VkLayer_dlssnr_win.json`). Intercepts `vkQueuePresentKHR`
of any Vulkan application, forces `TRANSFER_SRC|DST` on the swapchain image,
round-trips the frame to the daemon and writes the processed image back.
Registered in `HKCU\Software\Khronos\Vulkan\ImplicitLayers`; loads only when
`ENABLE_NR_LAYER=1` is set (manifest `enable_environment` gating), so games
launched outside the manager are never touched. x64 + x86 builds — the 32-bit
pair is what makes GTA IV (DX9, 32-bit via 32-bit DXVK) work.

Hotkeys handled by the layer: `CTRL+ALT+X` pause/resume (the flag-present path
passes frames through untouched at full fps), `CTRL+ALT+Q` unload.
`NR_LAYER_CAPTURE_OUT` dumps the processed frame for offline analysis.

Photo mode: `NR_LAYER_FREEZE` (env or `%TEMP%\m13_freeze.flag`) holds the raw
frame captured at freeze time and re-blits its processed result every present —
the game keeps running, the picture stands still. `NR_LAYER_REPROC` /
`m13_reproc.flag` re-sends the *same raw frame* so a knob turn reprocesses the
untouched original (validated end-to-end on vkcube: "frame frozen" → 4×
"reprocessed the held frame" → live resume).

### `dlss5/m11d` — frame daemon

TCP daemon on `127.0.0.1:47990`; protocol = 16-byte header
(`magic=0x304E524E`, width, height, VkFormat) + `w*h*4` BGRA payload in both
directions. The masked magic variant carries a held-still UI mask that passes
through untouched; input alpha is preserved. Lazy engine init per frame size
(first frame ~6 s — weights upload). `--selftest file.bmp` runs the one-shot
validation against the torch goldens. `--weights` is mandatory.

Runtime control channel (NRCT, same TCP port): `STATUS`, `SETGAIN`,
`SETBLEND` — this is how the manager and the in-game overlay retune the two
knobs live.

### `dlss5/m12-dxgi` — DX12 proxy

Proxy `dxgi.dll` deployed next to a DX12 game's executable: forwards all
exports to the system DLL, vtable-hooks `IDXGISwapChain::Present/Present1`,
does the same daemon round-trip as the Vulkan layer (readback → TCP → upload →
`CopyTextureRegion` back). `M12_LIVE=N` processes every Nth present and
re-blights the last processed frame in between. Same freeze/reprocess flag
mechanism as the layer.

### DX9/10/11 path

No custom D3D code at all: the manager deploys DXVK (`d3d9.dll` / `d3d11.dll` +
`dxgi.dll` + `d3d10core.dll`, both arches) into the game folder, the game runs
through Vulkan, and the implicit layer applies. Mode → DLL set mapping
(`m13/deploy.py`): `dx9={d3d9}` (never `dxgi` — mixing DXVK's dxgi with a
native d3d9 crashes, learned on GTA IV), `dx11={d3d11,dxgi,d3d10core}`,
`dx12={m12 proxy}`, `vulkan={}` (layer only).

### `dlss5/m8b-live` — screen/window mode

Standalone fullscreen app: DDA desktop duplication capture (M8c) → CPU bridge →
Vulkan: features → 71-block chain (~51 ms at its working size) → residual →
high-pass → `fbcancel` (echo subtract + bounded accumulate) → compose
(strength, clamp) → present to a topmost click-through overlay. Window mode
(`--window "title"`) captures a single window — small extents are near
real-time. Live knob file `%TEMP%\m13_screen_knobs.txt` (`gain blend`,
mtime-polled once per processed frame) — no restart needed to retune.

VRAM exclusivity: `m11d` and `m8blive` each reserve ~12 GB arena; the manager
stops one before starting the other (logged).

### `dlss5/m13` — the manager

Stdlib-only Python/Tk application (`m13.pyw` + modules):

| Module | Responsibility |
|---|---|
| `controller.py` | state machine; `StateMonitor` (single background probe thread, NRCT timeout 0.8 s) + `ActionWorker` (every mutation off the UI thread) — the Tk thread does zero I/O |
| `gamescan.py` | all-drive scan + Steam `libraryfolders.vdf` + Epic manifests + GOG registry; PE parsing (imports, delay-load directory, wide D3D strings) → real renderer exe + bitness + API; launcher wrappers resolved (`Launcher.exe` → real binary) |
| `deploy.py` | per-mode DLL deployment with backup/restore; `DeployNeedsElevation` → one UAC prompt runs an elevated copy for Program Files games |
| `gamelaunch.py` | launches through wrappers with `ENABLE_NR_LAYER=1` |
| `overlay.py` | frameless in-game panel (hotkey `CTRL+ALT+G`): pause/resume, gain + blend sliders, live status; focus-steal + cursor release against games that re-clip the cursor every frame |
| `screenmode.py` | start/stop `m8blive`, screen knobs via knob file |
| `daemonctl.py` | m11d lifecycle, autosetup (weights find, layer registration, daemon start) |
| `i18n.py` | all UI strings, EN + RU |
| `ui.py`, `ui_games.py`, `icons.py`, `splash.py`, `logtail.py`, `config.py`, `paths.py`, `processes.py` | dark-theme UI, icon cards, splash, log pane, config, paths, process probes |

Launcher chain: `DLSS5 Manager.vbs` (WScript, zero console, discovers
pyw/pythonw/PATH/per-user Python) → `M13.cmd` fallback → `m13.pyw` (stderr →
`logs\manager_err.log`, `icon.ico`, own AppUserModelID).

## Validation rig

- `m11d --selftest` vs PyTorch goldens: features 15/16 channels bit-exact,
  head `[TOK,16]` meandiff ≈ 0.0083 — the fp16 noise level, identical to the
  m8b reference.
- `blend=0` selftest: bit-exact passthrough (maxdiff 0 vs input).
- 45-frame `m8blive` PASS runs with verify-frame dumps compared per-frame.
- `_smoke.py` / `_ui_smoke.py` integration suites against a live daemon
  (deploy roundtrips, PE detection, knob pushes confirmed via NRCT STATUS).
- Golden-rule: anything touching the chain or shaders must be rebuilt and
  re-validated against the goldens before merge.

## Performance

Current numbers on the Arc Pro B50 (GEMM-bound; the shader GEMM reaches
~0.6–2.9 TF/s of the ~30 TF/s the hardware can do):

| Size | Chain time | Result |
|---|---|---|
| 500×500 | ~135–145 ms/frame | ~7 fps |
| 960×540 | ~700 ms/frame | slideshow |
| 1080p-class | ~0.9–1.1 s/frame | ~0.7–1 fps |
| 2560×1440 screen mode | tens of seconds for frame 0 | demo-only |

Known headroom (measured, documented in DEV_STATE.md): barrier tax ~276 ms at
1080p (addressed), arena layout single sparse buffer (kept), GEMM tuning is
the big remaining item — see `dlss5/m8b-live/shaders/m8/gemm.comp`. This is
the #1 open problem; contributions welcome.

## Repository map

- `dlss5/` — all code (m0–m13 prototypes; `chain/`, `m11-layer/`, `m11d/`,
  `m12-dxgi/`, `m13/`, `m8b-live/` are the shipping pieces)
- `docs/` — analysis, handoffs, logs, this document, the before/after viewer
- `release/` — release-bundle assembly script
- `AGENTS.md` / `DEV_STATE.md` — working rules and the day-by-day engineering
  log (written for AI coding agents working in this repo; humans can treat
  DEV_STATE.md as the lab notebook)
- Weights and `reference/` third-party code are **not** in this repository
  (gitignored by policy)
