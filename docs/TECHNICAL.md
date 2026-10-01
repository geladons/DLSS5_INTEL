# Technical reference

This document is the engineer-facing description of the current tree. It
covers module boundaries, the frame transport paths, the daemon protocol,
frame generation, weights handling and the known limitations of this
snapshot. For build steps see [BUILDING.md](BUILDING.md); for benchmark
numbers see [BENCHMARKS.md](BENCHMARKS.md); for the public bundle see
[RELEASING.md](RELEASING.md).

Status terminology used below:

- **verified in tree** - behavior confirmed by reading the current source.
- **live-tested** - additionally observed on the bench VM (see DEV_STATE.md).
- Where docs conflict, the source wins. Known stale spots are listed at the
  end ("Stale documentation").

## 1. Module map

| Module | Kind | Role |
|---|---|---|
| `dlss5/chain` | C++ library + `d5c_chain.dll` | The 71-block DLSSNR graph executor (cooperative-matrix GEMM on XMX). Exposes the C ABI all in-process consumers use. |
| `dlss5/m11-layer` | Vulkan implicit layer DLL | Intercepts `vkQueuePresentKHR`. x64 build can run the chain in-process; the x86 build (32-bit games) talks TCP to `m11d`. |
| `dlss5/m11d` | x64 Windows service exe | Frame daemon for the x86 game path. Listens on `127.0.0.1`, default port **48990**, runs the chain. |
| `dlss5/m12-dxgi` | Proxy `dxgi.dll` (x64) | Vtable-hooks `IDXGISwapChain::Present`/`Present1` for DX12 games; prefers in-process `d5c_chain.dll`, falls back to the TCP daemon. |
| `dlss5/m8b-live` | x64 app (`m8blive.exe`) | Screen/window mode: DDA capture -> chain -> click-through fullscreen Vulkan overlay. |
| `dlss5/m13` | Python/Tk manager | Setup, game discovery, deploy, launch, daemon lifecycle, overlay, screen mode, media tab, cleanup. Stdlib only. |
| `dlss5/m6-weights-loader` | tool | Standalone safetensors loader/validator (`--file`, `--verify`, `--stats`). |
| `dlss5/m0`-`m5`, `m7`-`m9` | prototypes | Milestone scaffolds kept for history; the shipping pieces are listed above. |

Core engine classes (`dlss5/chain`): `d5c::ChainEngine` (front-end),
`VkContext` (device + `VK_KHR_cooperative_matrix` checks), `WeightsStore`
(safetensors -> packed resident layout), `ChainArena` (buffer slots,
3.5 GiB chunking), `ChainRecorder` (pipeline/dispatch recording across the
71-block U-Net). See [ARCHITECTURE.md](ARCHITECTURE.md) for the pipeline
walk.

## 2. Frame transport paths

There are two ways a captured frame reaches the chain:

- **In-process (x64):** the consumer loads `d5c_chain.dll` and calls the C
  ABI directly. No socket, no daemon. This is the preferred path for the
  x64 Vulkan layer, the DX12 proxy and the media tab.
- **TCP daemon (x86 games / fallback):** one loopback connection per frame
  to `m11d` on `127.0.0.1:48990`.

### 2.1 Vulkan games (x64)

`nr_layer_win.dll` is an implicit Vulkan layer registered under HKCU.
Armed by `ENABLE_NR_LAYER=1` (manifest `enable_environment`), it hooks
`vkQueuePresentKHR`, forces `TRANSFER_SRC|TRANSFER_DST` on the swapchain
images, grabs the presented image into a host-visible staging buffer and
processes it.

On x64 the layer tries to `LoadLibraryA("d5c_chain.dll")` first
(`nr_layer_win.c`, in-process probe around lines 433-510; also looks next
to the layer DLL and at dev-tree build dirs). If the DLL or a required
export is missing it logs and falls back to the TCP path
(`nr_layer_win.c:484`, `:508`). `NR_LAYER_INPROCESS=0` forces TCP.

### 2.2 DX9 / DX10 / DX11 games

The manager deploys DXVK (`d3d9.dll` / `d3d11.dll` / `d3d10core.dll` /
`dxgi.dll`) next to the game binary, so the game renders through Vulkan and
the **same Vulkan layer** applies. Bit-ness matters:

- **x86 (32-bit) games** (GTA IV, GTA SA): the game is a 32-bit process, so
  the x86 layer build (`nr_layer_win32.dll`, built by
  `dlss5/m11-layer/_build_x86.cmd`) is what the Vulkan loader picks up via
  `Wow6432Node` registrations. A 32-bit process cannot load the x64
  `d5c_chain.dll`, so this path is **TCP-only** to the `m11d` daemon. The
  daemon itself is an x64 exe - "x86 path" describes the game, not the
  daemon.
- **x64 games** get the DXVK x64 set plus the x64 layer, i.e. the
  in-process path.

### 2.3 DX12 games

`m12_dxgi.dll` is a proxy `dxgi.dll` placed next to the game binary. It
forwards all real DXGI exports to the system DLL and vtable-hooks
`IDXGISwapChain::Present`/`Present1`. Like the layer it prefers the
in-process engine (`ensure_inprocess()` in `m12_dx12.cpp`) and falls back
to the TCP daemon with a 2 s socket timeout (`m12_client.cpp:43-45`).

`M12_LIVE=N` paces how often frames are processed; pause/resume uses the
`%TEMP%\m12_pause.flag` file. `M12_*` env vars mirror the `NR_LAYER_*` /
`DLSS5_*` knobs (see section 6).

### 2.4 Screen / window mode

`m8blive.exe` captures the desktop or a chosen window through DXGI Desktop
Duplication, feeds frames through the chain on its own Vulkan device and
presents the result through a click-through, topmost Vulkan overlay window.
It does **not** use the daemon and runs its own engine instance - which is
why daemon and screen mode cannot run at the same time on a 16 GB card
(~12 GB reserved each; the manager enforces mutual exclusion).

**Known limitation (verified in tree):** the weights path in
`dlss5/m8b-live/main.cpp:1831` is a hard-coded absolute path
(`stParse("C:\\Users\\AI\\Desktop\\DLSS5_INTEL\\work\\mlxw\\dlssnr-logical.safetensors", ...)`).
There is no `--weights` flag and no env override; `stParse` exits the
process if the file cannot be read. Screen mode therefore only works on
machines where that exact path exists - on a fresh install of the public
bundle it will fail unless the user recreates the path. This is a
portability gap, not a spec.

### 2.5 Media tab

`m13/media.py` wraps `d5c_chain.dll` through ctypes (`EngineSession`) and
processes photos (PNG/BMP, JPEG when ffmpeg is present) and videos
in-process at native scale (media scale is pinned to 1.0). Video is decoded
frame-by-frame through ffmpeg/ffprobe pipes and remuxed keeping the
original fps and audio (`media_video.py`). ffmpeg can be downloaded
on demand from the tab.

## 3. Daemon wire protocol

Verified against `dlss5/m11d/main.cpp` (header comment lines 6-19, command
dispatch lines 445-530) and the client `dlss5/m13/daemonctl.py`.

- Endpoint: `127.0.0.1`, default port **48990** (`--port`, or env
  `NR_LAYER_PORT` / `DLSS5_PORT`). Port 47990 in older docs is stale -
  Sunshine occupies 47990 on the bench host, which is why the daemon moved
  (DEV_STATE.md).
- Framing: 16-byte header of four little-endian `uint32`
  `{magic, w, h, format}`. One TCP connection per frame or request; the
  reply for frame traffic is exactly `w*h*4` bytes of BGRA.

| Magic (u32 LE) | Direction | Payload |
|---|---|---|
| `0x304E524E` | frame | `w*h*4` BGRA bytes |
| `0x314E524E` | frame | BGRA payload + `w*h` mask bytes (held-still UI hint; the daemon still accepts it for protocol compatibility - the current layer no longer sends it since HUD masking was removed) |
| `0x5443524E` ("NRCT") | control | 16-byte header `{magic, cmd, payload, reserved}` |

NRCT commands (all produce a 16-byte reply `{magic, ok, gainBits, frames}`
- the reply carries the *active* gain and the processed-frame counter so a
caller can confirm a live change):

| cmd | Name | Payload | Verified at |
|---|---|---|---|
| 1 | SETGAIN | float bits | `m11d/main.cpp:445`, `daemonctl.py:20` |
| 2 | STATUS | none | `daemonctl.py:21` |
| 3 | SETBLEND | float bits 0..1 | `m11d/main.cpp:458` |
| 4 | SETSCALE | float bits | `m11d/main.cpp:472` |
| 5 | SETFRAMEGEN | int 0/1 | `m11d/main.cpp:486` |
| 6 | SETLOCALTONE | float bits | `m11d/main.cpp:490` |
| 7 | SETLOCALSTRUCTURE | float bits | `m11d/main.cpp:497` |
| 8 | SETSKINSTRUCTURE | float bits | `m11d/main.cpp:504` |
| 9 | SETAUTOMASK | int 0/1 | `m11d/main.cpp:511` |
| 10 | SETSTYLEINDEX | int 0/1/2 | `m11d/main.cpp:517` |
| 11 | SETCOLORSTRENGTH | float bits | `m11d/main.cpp:523` |

Timeouts: daemon-side sockets use 20 s send/recv (`m11d/main.cpp:434-436`,
"a full-extent chain can take ~1 s"); the Vulkan layer exchange timeout is
1.5 s (`nr_layer_win.c:581-583`); the DX12 client uses 2 s
(`m12_client.cpp:43`); the manager's NRCT client uses 5 s for UI actions
(`daemonctl.py:42`) and 0.8 s for the background monitor
(`controller.py:37`, `MONITOR_PERIOD_S = 1.0`).

Probe caveat: a raw TCP connect can succeed without `m11d` (anything may be
listening), so the manager only trusts a reply whose magic is NRCT
(`daemonctl.py:12-13`, `:64-67`).

## 4. Frame generation

Two implementations exist in the C ABI (`chain_c_api.h`):

- `dlss5_framegen_transfer` - **the active path**, residual motion
  transfer: `generated = cur_raw + warp(prev_processed - prev_raw,
  motion(prev_raw -> cur_raw))` (`dlss5/m8b-live/shaders/framegen.comp`
  mode 1). ~0.05 ms at 640x384 on the bench GPU. Static scenes reproduce
  the processed frame bit-exactly; no extra present and no extra latency.
- `dlss5_interpolate_frame` - the **legacy** mode-0 interpolator that
  blends two frames with a factor. Kept for compatibility.

Per-consumer reality (do not generalize):

- **x64 Vulkan layer** (`nr_layer_win.c` + `nr_framegen.c`): with FG
  enabled and the in-process engine loaded, the layer runs a real 1:2
  cadence - every second present goes through the network, the present in
  between is produced by `dlss5_framegen_transfer`. `nr_framegen.c` tracks
  `presents/processed/generated/native` and logs stats every 300 presents.
  If the in-process engine is unavailable FG is reported unavailable and
  skipped presents go through native.
- **x86/daemon path:** no frame generation - the TCP protocol carries
  single frames only; the layer logs "requires in-process x64 engine" once.
- **DX12 proxy (`m12_dx12.cpp`):** still calls the **legacy**
  `dlss5_interpolate_frame` (loaded at line 205, alpha 0.5) on skipped
  presents, not residual transfer. This is a known divergence.
- **Daemon (`m11d`):** accepts NRCT cmd 5 and stores the flag
  (`main.cpp:486-489`) but the TCP path performs no generation.

## 5. Render scale and residual reconstruction

`scale < 1.0` (Performance/Quality presets) is **not** a bilinear blow-up
of a small output. The network runs at the reduced extent, `compose`
emits the *signed* delta between network output and the low-res input, the
delta is upscaled, and `encode` mode `b.w = 2` adds it onto the
full-resolution native frame. Native text/HUD/edge detail is preserved.
The deterministic vendor noise seed was pinned (`p.b[0] = 1`) to remove
temporal shimmer on static content; `chain/test_noise_seed.py` is the
regression test.

`scale = 1.0` is a separate code path (full-extent processing, no residual
reconstruction).

## 6. Runtime configuration surface

The manager writes these env vars into every launched game
(`dlss5/m13/gamelaunch.py`, assembled at lines 140-318). Consumers read
them at engine init / per frame.

| Variable(s) | Meaning |
|---|---|
| `DLSS5_WEIGHTS_PATH` / `NR_LAYER_WEIGHTS` | absolute path to `dlssnr-logical.safetensors` - passed to *every* launch since Stage 25 (previously only the daemon got `--weights`, which killed x64 in-process games at frame 0) |
| `DLSS5_SHADER_DIR` / `NR_LAYER_SHADERS` | directory with the compiled `.spv` kernels |
| `DLSS5_SCALE` / `NR_LAYER_SCALE` / `M12_SCALE` | render scale preset (1.0 / 0.67 / 0.58 / 0.50 / 0.33) |
| `DLSS5_GAIN` / `NR_LAYER_GAIN` / `M12_GAIN` (+ `*_INTENSITY`) | effect intensity 0..16 |
| `DLSS5_BLEND` / `NR_LAYER_BLEND` / `M12_BLEND` | effect mix 0..1 (0 = bit-exact passthrough) |
| `DLSS5_FRAMEGEN` / `NR_LAYER_FRAMEGEN` | frame generation on/off |
| `DLSS5_SHARPNESS` / `NR_LAYER_SHARPNESS` | CAS sharpening 0..1 |
| `ENABLE_NR_LAYER` | arms the implicit layer (manifest-gated) |
| `NR_LAYER_LIVE` / `NR_LAYER_TRIGGER` | live mode + trigger-flag path (DX9-style pause: processing runs while the flag file *exists*) |
| `NR_LAYER_FREEZE` / `NR_LAYER_REPROC` | photo-mode freeze / reprocess flag paths |
| `NR_LAYER_UI_MASK` | `0` disables exact-repeat reuse of a byte-identical frame (legacy name; HUD masking itself was removed in Stage 25) |
| `NR_LAYER_INPROCESS` | `0` forces the TCP path even when `d5c_chain.dll` is loadable |
| `NR_LAYER_PORT` / `DLSS5_PORT` | daemon port override (default 48990) |

Flag files (all under `%TEMP%`): `m13_dx9_trigger.flag` (layer pause),
`m12_pause.flag` (DX12 pause), `m13_freeze.flag` / `m13_reproc.flag`
(photo mode), `m13_screen_knobs.txt` (screen-mode knobs),
`m13_phase30_knobs.json` (Phase30 tuning set).

Phase30 tuning (intensity, local tone, local structure, skin structure,
auto-mask, style index, color strength) maps 1:1 to NRCT commands 1-11 and
to `dlss5_set_phase30` / the individual setters in the C ABI.

## 7. Weights

- Required file: `dlssnr-logical.safetensors` - Safetensors, layout tag
  `dlssnr-logical-v18`, 649 tensors (579 F16 + 70 F32) in 71 top-level
  block groups, ~291.5 MB on disk.
- SHA-256:
  `203b0af3be94078cfd17a71adc4628cffd960a4d435e4820fe994ddd9493a6a5`
  ([weights-format.md](weights-format.md), inventory in
  `weights-inventory.txt`, provenance in `weights-provenance.md`).
- Optional INT8 pack `dlssnr-int8.bin` is auto-detected next to the logical
  weights or under `work/mlxw`; it drives the fused INT8 kernels with
  rollback envs (`D5C_INT8_ATTN_FUSED=0`, `D5C_INT8_POOL_VEC=0`).
- The chain keeps roughly ~325 MB of packed weights resident (arena
  reservations are much larger - do not conflate the two).
- **Distribution policy:** weights are NVIDIA proprietary data. They are
  not in git (gitignored `*.safetensors`), not in the public bundle
  (`assemble-release.cmd` excludes them explicitly), and must never be
  attached to a release. The private production folder does contain them
  for local use - see [RELEASING.md](RELEASING.md) for the distinction.

## 8. Manager internals (`dlss5/m13`)

- `m13.pyw` - entry point (Tk, stdlib only, EN/RU via `i18n.py`).
- `launcher.py` - `GameLauncher`: every launch is a dedicated `m13-launch`
  thread of timed steps (guard_busy -> register -> resolve ->
  guard_running -> deploy -> daemon -> spawn), one `[launch] game=..
  step=.. ok=..` line per step in `manager_err.log`. Refuses double launch,
  an already-running game, or a second managed game; never mutates config
  mid-launch.
- `worker.py` - `ActionWorker` serializes all mutating actions on one
  queue; `WorkerSupervisor` (period 5 s) abandons a job older than
  `JOB_TIMEOUT_S = 150.0` and starts a fresh worker on the same queue, so a
  stuck job can no longer freeze the UI.
- `controller.py` - `StateMonitor` probes daemon status once per second
  with a 0.8 s NRCT timeout; UI never blocks on I/O.
- `deploy.py` - per-game DLL deploy/undeploy (DX9 = `d3d9.dll`-only rule),
  backups before overwrite, HKCU Vulkan-layer registration via `winreg`
  (no `reg.exe` quoting problems), one UAC elevation for Program Files.
- `cleaner.py` - "Remove all traces": restores parked originals
  (including sibling dirs), unregisters layers, stops the daemon, clears
  flag files/logs/config. Foreign DLLs are reported, never deleted.
- `gamescan.py` / `gamescache.py` - Steam/Epic/GOG manifests + full-drive
  PE scan (imports, delay-load, wide D3D strings) with an on-disk cache.
- `paths.py` - production layout (`runtime\...`) first, dev-tree layout
  (`dlss5\...`) fallback - the same code runs from the repo and from the
  bundle.
- `screenmode.py`, `overlay.py`/`overlay_win32.py`, `media*.py`,
  `daemonctl.py`, `gamelaunch.py`, `phase30.py`, `logtail.py`,
  `processes.py`, `config.py`, `ui*.py` - see module docstrings
  (`__init__.py` has the full one-line index).
- Tests (run from `dlss5\`): `python -m m13.test_cleaner`,
  `test_launch_paths.py`, `test_launcher.py`, `test_phase30_cache.py`,
  `test_stage5.py`, `test_media.py`, `test_env.py`, plus `_smoke.py` /
  `_ui_smoke.py` integration smokes.

## 9. Startup and failure behavior

- `dlss5_create` never kills the host: engine init can fail or throw; both
  paths are caught, logged and latched (`chain_c_api.cpp:127-149`), and
  `processFrame` returns `-1` so the consumer presents the native frame
  instead of dying. A failed init is not retried per frame for the same
  config; a config change allows one fresh try.
- `m11d` requires `--weights` (`main.cpp` exits with usage otherwise) and
  supports `--selftest file.bmp` (golden-vs-chain validation against the
  torch dumps in `work/_m9b_cmp`) and `--bench N WxH [--prof out.csv]`.
- Daemon flap handling on the TCP path (`nr_degrade.c/.h`): a transient
  exchange failure holds the last processed frame of the same extent; a
  never-answering daemon stays native; degenerate sub-64 px swapchains
  (menu strips) bypass the daemon entirely.
- First frame is slow by design: ~30 s weights upload plus up to ~60-70 s
  for frame 0 at high resolution. That is warm-up, not a hang.
- Overlay (`overlay_win32.py`): topmost `WS_EX_NOACTIVATE` panel; photo
  freeze (`overlay_freeze`) and focus stealing (`overlay_focus`) are both
  **opt-in and off by default** - opening the overlay does not freeze the
  game unless freeze was enabled.

## 10. Performance data (read carefully)

Numbers in this repo come from different builds and measurement modes -
they are not interchangeable. Verified current values, Intel Arc Pro B50,
Vulkan 1.4.348 / driver 101.8805:

| Measurement | Value | Where recorded |
|---|---|---|
| Scale 0.50, 960x576, v0.3 INT8 fused | 214.38 ms / 4.66 FPS | `docs/BENCHMARKS.md` fused table |
| Scale 0.50, 960x576, v0.3 steady | 210.2 ms / 4.76 FPS | `ARCHITECTURE.md`, README (same build family, different run) |
| Scale 1.00, 1920x1088, v0.3 INT8 fused | 741.83 ms / 1.35 FPS | `docs/BENCHMARKS.md` |
| Scale 1.00, 1920x1088, v0.3 | 547 ms selftest / 767.5 ms steady | `ARCHITECTURE.md`, `DEV_STATE.md` |
| Ultra Performance 640x384 | 125.5 ms / 7.97 FPS | `BENCHMARKS.md` |
| FG residual-transfer GPU cost | ~0.05 ms @640x384 (0.39 ms in older notes) | `BENCHMARKS.md`, `DEV_STATE.md` |
| v0.1-demo reference | ~135 ms @500x500; ~0.9-1.1 s at 1080p-class | `CHANGELOG.md` known-limitations |

"selftest" numbers are one-frame `--selftest` timings; "steady" numbers are
sustained-loop timings; "fused" numbers come from the INT8 fused-kernel
build. Label any new measurement the same way.

## 11. Known limitations

- **Slideshow performance.** Even the fastest preset is ~5-10 FPS with FG;
  this is a research demo, not a playable tool.
- **m8blive weights path is hard-coded** to the dev tree
  (`main.cpp:1831`) - see section 2.4. Screen mode is effectively
  dev-machine-only in the current snapshot.
- **VRAM exclusivity.** Daemon and screen mode each reserve ~12 GB; on the
  16 GB bench card they cannot run at the same time (manager-enforced).
- **DX12 FG divergence.** The proxy uses the legacy interpolator, not
  residual transfer (section 4).
- **x86 path has no FG** and pays full TCP transport per frame.
- **No HUD masking.** Positional masking was removed in Stage 25; every
  pixel of a processed frame goes through the network. Exact-repeat reuse
  (byte-identical frame reuses the last result) is the only passthrough.
- **Golden drift.** Head meandiff sits at ~0.0081-0.0088 vs the torch
  goldens (expected ~0.008) - fp16 noise level, tracked in DEV_STATE.md;
  selftest PSNR ~39.4 dB (INT8) / ~44.7 dB (FP16).
- **Anti-cheat.** Any injectable game with kernel anti-cheat can read the
  deployed DLLs as cheating. Single-player only.

## 12. Stale documentation (do not copy forward)

- TCP **47990** appears in `ARCHITECTURE.md`, `BUILDING.md`, the README
  diagrams and the v0.1 bundle. The current default everywhere is **48990**
  (Sunshine collision; `NR_LAYER_PORT`/`DLSS5_PORT`/`--port` override).
- "0 ms socket lag" for x64 paths means *no TCP transport* (in-process C
  ABI), not zero processing cost - in-process front-end transport measures
  ~1.5-2.5 ms.
- Older README/handoff wording implies opening the overlay freezes the
  frame; freeze is now opt-in (`overlay_freeze`, default off).
- Historical handoffs claim packaged INT8 weights in the *public* bundle;
  the policy is and stays: no weights in public artifacts.
