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

NRCT commands (all produce a 16-byte reply `{magic, ok, valueBits, frames}`
- the reply echoes the knob the command touched, or the residual gain for
STATUS/valueless commands):

| cmd | Name | Payload | Engine slot |
|---|---|---|---|
| 1 | SETGAIN | float bits 0..16 | residual multiplier (`headGain`, compose `pc.c.z`) |
| 2 | STATUS | none | — |
| 3 | SETBLEND | float bits 0..2 | compose mix (`pc.c.w`, same slot as intensity) |
| 4 | SETSCALE | float bits | render scale (engine re-init) |
| 5 | SETFRAMEGEN | int 0/1 | frame generation on/off |
| 6 | SETLOCALTONE | float bits | Phase30 feature channel |
| 7 | SETLOCALSTRUCTURE | float bits | Phase30 feature channel |
| 8 | SETSKINSTRUCTURE | float bits | Phase30 feature channel |
| 9 | SETAUTOMASK | int 0/1 | Phase30 feature channel |
| 10 | SETSTYLEINDEX | int 0/1/2 | Phase30 feature channel |
| 11 | SETCOLORSTRENGTH | float bits | Phase30 color strength |
| 15 | SETINTENSITY | float bits 0..2 | Phase30 compose mix (`pc.c.w`) |

`gain` and `intensity` are independent knobs on every path: `gain` is the
residual multiplier (user-facing effect strength, neutral 1.0),
`intensity` is the Phase30 compose mix (public baseline 1.0).

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

Two algorithms live behind `NrFrameGen` (`nr_framegen.h/.c` in the layer)
plus the daemon-side NRN2/NRN3 wire formats:

- **Mode 2 (default for opt-in FG, `*_FG_MODE=2`, LS-class 1:2 cadence):**
  on the mid present the layer captures the REAL middle frame into
  `data->mid` and re-blits the held processed anchor; on the anchor
  present it calls `dlss5_framegen_interpolate` (x64 in-process) or sends
  the mid payload as an NRN3 packet to `m11d` (x86/daemon). The generated
  midpoint of the last two processed anchors goes out at a uniform
  +1-present delay.
- **Mode 1 (`dlss5_framegen_transfer`, NRN2, legacy +0-latency):**
  residual motion transfer -
  `generated = cur_raw + warp(prev_processed - prev_raw,
  motion(prev_raw -> cur_raw))` (`framegen.comp` mode 1, ~0.05 ms at
  640x384). Kept for latency-critical games.

Per-consumer reality (do not generalize):

- **x64 Vulkan layer** (`nr_layer_win.c` + `nr_framegen.c`): with FG
  enabled and the in-process engine loaded, the layer runs the mode-2
  cadence in-process; `nr_framegen.c` tracks
  `presents/processed/generated/native` and logs stats every 300
  presents (`fg stats`, `diag_path=fgi/mheld` in
  `%TEMP%\nr_layer_win.log`).
- **x86/daemon path:** mode 2 works over TCP - the mid payload crosses as
  an NRN3 packet and `m11d` runs the same interpolation engine-side.
- **DX12 proxy (`m12_dx12.cpp`):** still calls the **legacy**
  `dlss5_interpolate_frame` (alpha 0.5) on skipped presents - a known
  divergence from the mode-2 path.

## 4b. Temporal stability pipeline (`temporal_nr` / `temporal_stats` / `motion_est`)

Every processed frame passes a temporal stage between the 71-block head
and `compose`. Its job: keep static output byte-exact, carry enhancement
residual WITH the content under motion, and never let a stale residual
survive an input change. `dlss5/chain/engine_temporal.cpp` drives it;
`tools/autotest/replay_gate.py` is the release gate.

- **Residual-domain hold (v9):** per pixel the state is
  `rh = E - P` (last emitted byte minus the input it was produced on).
  Held emit = `col01 + rh` stepped toward the fresh residual by
  `w = max(wA(dF), wR(dr), BETA*|mv|)` inside a budgeted refresh
  (`R_FRAMES=16`, proportional steps, still-pixel cap `D5C_RF_SCAP`).
  A fresh emit (input jump > `IN_CUT`, residual mismatch >= `DB_CUT`,
  reset frame) re-anchors outright. Static input reproduces the previous
  emit **byte-exactly** - the fixed point that pins the gain>1 period-2
  limit cycle. Never reintroduce an output-domain latch: it ghosts
  moved/faded content (`fixture:ghost` gate).
- **Emit deadband (v14):** sub-LSB input jitter is pinned via `E_HOLD`
  and P is frozen while pinned, so dithered input cannot random-walk E
  upward (the "litter accrues while still" defect class).
- **Motion-compensated state (v10):** `motion_est.comp` runs one
  workgroup per 8x8 block - full integer SAD search (+-12 px), zero-motion
  bias, parabolic subpixel refine. E/P/A/R are fetched at `q = p - mv`
  (integer verbatim, fractional bilinear on decoded bytes), so the
  carried residual belongs to content, not screen coordinates.
- **DC-correct still-match (v15):** the block's (0,0)-tap diff minus its
  block mean (DC) is re-scored as `s0dc`; if `s0dc <= SADmin` the change
  is a brightness ramp (fade/exposure/flash), not displacement, and the
  block is pinned (0,0). Removes ~97% of false motion on fades while
  keeping ~90% of real camera-motion blocks moving - without it every
  fade charged the motion envelope and produced a ~3 s post-transition
  enhancement crawl.
- **Motion-envelope residual attenuation (v13+v16):**
  `env = max(|mv|, MREL*env_prev)` per block (fast attack, MREL=0.95
  release) scales the OUTPUT residual by `g = 1 - MA*smoothstep(M0,M1,env)`
  to hide a carried residual that lands on wrong content. History E
  stores the UNattenuated emit. Since v16 the attenuation multiplies by
  `smoothstep(RLO, 2.5*RLO, dr)` - it releases on **residual
  verification** per pixel instead of the ~60-frame envelope clock, so
  local motion (cursor sweep, a moving prop) no longer leaves a visible
  ~3 s under-enhanced trail that re-densifies.
- **History sanitization:** `features.comp` AABB-clamps the history input
  (ch 7-9) into the current 3x3 neighbourhood (+-0.004 scaled) - kills
  cursor trails and stale-region drag. `temporal_nr` additionally gates
  the vendor blend alpha per pixel by the clamped/unclamped distance
  (fv[15]). All prior-state reads go through `bufHistPrev`, never the
  live buffer being rewritten.
- **Scene cuts:** the host thumb pass (`detectSceneCut`) plus per-pixel
  `dF > IN_CUT` reset history influence; mode-2 suppression frames are
  shader-equivalent to mode-1 so re-engagement is transient-free.

Release gates (must PASS on every temporal change):
`fixture:ghost`, `D:\dlss5_cap\_pan_seq.raw`, `Temp\_pan_man.txt`
(plus `_nat_load.raw` check). See AGENTS.md for the full rule text.

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
| `DLSS5_GAIN` / `NR_LAYER_GAIN` / `M12_GAIN` | residual gain multiplier 0..16 (`headGain`, neutral 1.0) |
| `DLSS5_INTENSITY` / `NR_LAYER_INTENSITY` / `M12_INTENSITY` | Phase30 compose mix 0..2 (`pc.c.w`, baseline 1.0) |
| `DLSS5_BLEND` / `NR_LAYER_BLEND` / `M12_BLEND` | effect mix 0..1 (0 = bit-exact passthrough) |
| `DLSS5_FRAMEGEN` / `NR_LAYER_FRAMEGEN` | frame generation on/off |
| `DLSS5_SHARPNESS` / `NR_LAYER_SHARPNESS` | CAS sharpening 0..1 |
| `ENABLE_NR_LAYER` | arms the implicit layer (manifest-gated) |
| `NR_LAYER_LIVE` / `NR_LAYER_TRIGGER` | live mode + trigger-flag path (DX9-style pause: processing runs while the flag file *exists*) |
| `NR_LAYER_FREEZE` / `NR_LAYER_REPROC` | photo-mode freeze / reprocess flag paths |
| `NR_LAYER_UI_MASK` | `0` disables exact-repeat reuse of a byte-identical frame (legacy name; HUD masking itself was removed in Stage 25) |
| `NR_LAYER_INPROCESS` | `0` forces the TCP path even when `d5c_chain.dll` is loadable |
| `NR_LAYER_PORT` / `DLSS5_PORT` | daemon port override (default 48990) |
| `DLSS5_FG_MODE` / `NR_LAYER_FG_MODE` | frame-gen algorithm: `2` = LS-class mid-capture cadence (default), `1` = legacy residual transfer |
| `NR_LAYER_ASYNC` / `NR_LAYER_RESIDUAL` / `NR_LAYER_GPUWARP` | x86 serving mode: shipped `0/1/1` = synchronous daemon serve with masked-delta residual warp; `ASYNC=1` alone reverts to whole-stale-frame mailbox replay (ghosting) |
| `DLSS5_TEMPORAL` / `NR_LAYER_TEMPORAL` / `D5C_TEMPORAL` | `0` disables the temporal stage entirely (section 4b) |
| `D5C_RF_*` / `D5C_MV_*` | engine-side temporal knobs (refresh bands, motion estimation, envelope attenuation) - full list + defaults in the `engine.cpp` init log and `AGENTS.md` |

Flag files (all under `%TEMP%`): `m13_dx9_trigger.flag` (layer pause),
`m12_pause.flag` (DX12 pause), `m13_freeze.flag` / `m13_reproc.flag`
(photo mode), `m13_screen_knobs.txt` (screen-mode knobs),
`m13_phase30_knobs.json` (Phase30 tuning set).

Phase30 tuning (intensity, local tone, local structure, skin structure,
auto-mask, style index, color strength) maps 1:1 to NRCT commands 1-11 and
to `dlss5_set_phase30` / the individual setters in the C ABI.

## 6b. In-layer UI shared-memory ABI (`Local\DLSS5_UI_<pid>`)

The in-game overlay is rendered into the frame by the layer itself -
no sockets, no pipes. m13 creates a pagefile section named
`Local\DLSS5_UI_<game pid>`; the layer opens it lazily (one attempt per
present, `OpenFileMappingA` read/write) and blends the published bitmap
into `data->mapped` at the single `transfer(1)` write-back point, so
processed/held/generated/frozen frames all carry the UI. Absent mapping,
bad magic, hidden flag or stale heartbeat = zero behaviour.

Layout (all little-endian, `nr_ui.h` / `overlay_shm.py`):

| Offset | Field | Size / meaning |
|---|---|---|
| 0 | header | 64 B, `u32[16]`: `[0]` magic `0x3049554E` (`NUI0`), `[1]` seq (odd = mid-write), `[2]` bmp w, `[3]` bmp h, `[4]` dst x, `[5]` dst y, `[6]` flags (`1` visible, `2` show cursor), `[7]` cursor x, `[8]` cursor y, `[9]` heartbeat, `[10]` evt head, `[11]` evt tail, `[12]` dropped events |
| 64 | event ring | 256 x 16 B `{u32 type; i32 a; i32 b; u32 c}`; types 1 move, 2 btn down, 3 btn up, 4 wheel, 5 key down, 6 key up, 7 char |
| 4160 | cursor sprite | 32 x 32 RGBA straight-alpha |
| 8256 | bitmap | BGRA straight-alpha, max 1024 x 1200 |

Total size 4,923,456 B. The producer holds `seq` odd while it writes the
bitmap then bumps it to even; the reader snapshots header fields under a
bounded seqlock. `heartbeat` must change at least every 2 s
(`NR_UI_HB_STALE_MS`) - a stalled writer hides the panel and releases
the input capture rather than leaving a ghost UI that still eats input.

Input capture lives in the existing GDI-guard wndproc
(`gdi_guard.c`): while `flags&1` and the heartbeat is fresh the panel
eats mouse/keyboard messages (incl. `WM_INPUT` raw deltas for the
logical cursor) and pushes them to the ring; the game window is
ClipCursor'd to the panel rect and the cursor sprite is composited at
the tracked position. Focus/capture messages always pass through - the
overlay never manufactures focus transitions for the game. Teardown
(`nr_unguard_all` on instance destroy/detach) releases ClipCursor and
unmaps the view.

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
| **Live game: GTA IV 800x600, scale 0.67** | **~45 ms engine round-trip / 23.11 FPS avg in-game benchmark (37 s)** | production `m11d` log, `DEV_STATE.md` Stage 51 |
| FG residual-transfer GPU cost | ~0.05 ms @640x384 (0.39 ms in older notes) | `BENCHMARKS.md`, `DEV_STATE.md` |
| Engine arena VRAM (weights resident) | ~0.80 GB FP16 pack / ~3.87 GB INT8 pack | `m11d` startup log |
| v0.1-demo reference | ~135 ms @500x500; ~0.9-1.1 s at 1080p-class | `CHANGELOG.md` known-limitations |

"selftest" numbers are one-frame `--selftest` timings; "steady" numbers are
sustained-loop timings; "fused" numbers come from the INT8 fused-kernel
build; "live game" numbers are daemon round-trips inside a real present
loop (capture + features + chain + temporal + readback). Label any new
measurement the same way.

## 11. Known limitations

- **Retro/low-res is playable; 1080p-class is not.** GTA IV 800x600 @
  scale 0.67 runs at a ~45 ms engine round-trip (23 FPS avg in the
  in-game benchmark). 960x576+ stays in the ~4-9 FPS band even with FG;
  treat native-res modern titles as a research demo.
- **m8blive weights path is hard-coded** to the dev tree
  (`main.cpp:1831`) - see section 2.4. Screen mode is effectively
  dev-machine-only in the current snapshot.
- **VRAM exclusivity.** The engine arena reserves ~0.8 GB (FP16 pack) or
  ~3.9 GB (INT8 pack) with weights resident - several live instances now
  coexist on the 16 GB bench card; the manager still serializes daemon
  vs screen-mode runs as a policy.
- **DX12 FG divergence.** The proxy uses the legacy interpolator, not
  the mode-2 cadence (section 4).
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
- "~12 GB VRAM reserved" was the pre-`ChainArena` per-buffer allocation;
  the current engine arena measures 802.0 MB (FP16) / 3873.3 MB (INT8)
  with weights resident - `m11d` startup log is the source of truth.
- "x86 has no frame generation" is pre-mode-2 documentation: NRN3 mid
  payloads make FG work over the daemon path too.
- `NR_LAYER_PATCH` defaults OFF (rejected 2026-10-09 - the raw bbox is
  visibly unprocessed); older notes describe it as shipping-on.
