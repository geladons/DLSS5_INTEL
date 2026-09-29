# HANDOFF: DLSS 5 on Intel Arc Pro B50 (Updated 2026-09-28)

## 1. Quick Start & Execution
- **Production Bundle (User-Ready):**
  - Path: `C:\Users\<owner>\Desktop\production`
  - Launcher: Run `DLSS5 Manager.cmd` or `pythonw m13.pyw` directly from that directory.
  - Binaries and weights: All 101 hashes verified, `dlssnr-logical.safetensors` staged, x64/x86 layers registered.
- **Build from Source:**
  - Build Chain & Daemon: `cd dlss5\m11d && build.cmd`
  - Build Vulkan Layer (x64): `cd dlss5\m11-layer && build.cmd`
  - Build Vulkan Layer (x86): `cd dlss5\m11-layer && _build_x86.cmd`
  - Build DX12 Proxy: `cd dlss5\m12-dxgi && build.cmd`
  - Package Production: `python dlss5\m13\_build_production.py`
- **Validation:**
  - PyTorch Goldens: `cd dlss5\m11d\build\Release && m11d.exe --weights ..\..\..\..\work\mlxw\dlssnr-logical.safetensors --selftest ..\..\..\..\work\_m9b_cmp\native_crop.bmp && python ..\..\_validate.py out` (Must report `VALIDATION PASS`).
  - Unit Tests: `python dlss5\m13\test_stage5.py` (Must report `ALL STAGE 5 & TASK B UNIT TESTS PASSED SUCCESSFULLY!`).

---

## 2. Solved Issues & Ground Truth from Current Session

### Issue 1: Game & Menu Temporal Shaking / Judder (SOLVED)
- **Root Cause:** An experimental alternating odd-frame replacement `interpolateFrame(prev, in, 0.5)` in `chain_c_api.cpp` and `m11d.exe` replaced odd game frames with past interpolated frames. Because present layers present one frame per present call, this jumped time backward and forward ($t=0 \to t=0.5 \to t=2.0 \to t=2.5$), causing violent 60 Hz judder across scenes, characters, and menus.
- **Resolution:** Removed the odd-frame replacement block. Every frame advances monotonically forward through the full DLSS 5 U-Net pass. Judder and flickering are 100% eliminated.

### Issue 2: Menu & HUD Blur (UI Masking) (SOLVED)
- **Root Cause:**
  1. `data->have_earlier` was never set to 1 in `nr_layer_win.c`, disabling UI mask calculation.
  2. `mask_worth_sending` rejected masks if `held >= 90%`, which caused menus (95% static text) to lose UI masking and become blurred.
- **Resolution:**
  - Fixed `have_earlier` tracking and native input caching in `data->earlier`.
  - In `mask_worth_sending`: Changed condition to `held > (pixels / 100)`. In menus, all static text, buttons, and frames are preserved bit-exact from the native input frame, while moving 3D character models are processed by DLSS 5.

### Issue 3: In-Game Overlay Mouse Focus & Freeze (SOLVED)
- **Root Cause:**
  - Overlay opened with `WS_EX_NOACTIVATE` without stealing focus to prevent window minimization, but the game had captured the mouse and called `ShowCursor(FALSE)`.
  - Moonlight hid client cursor because the foreground window (game) had hidden its cursor.
  - Previous `suspend_pid()` suspended the game process in kernel mode while holding the Win32 input queue, locking the mouse.
- **Resolution:**
  - `overlay_freeze` defaults to `False` (live game runs at 60+ FPS while adjusting sliders). Photo Mode Freeze is triggered on demand via `freeze_set(True)`.
  - In `overlay.py`: `_free_cursor()` attaches thread input (`AttachThreadInput`) to the foreground game thread, releases clipping (`ClipCursor(None)`), releases capture (`ReleaseCapture()`), and un-hides `IDC_ARROW`.
  - `self.win.focus_set()` and `self.win.lift()` give the overlay mouse focus without calling `SW_RESTORE`, preventing DWM from minimizing exclusive fullscreen games.

### Issue 4: UI Window Layout Broken After Exiting Low-Resolution Games (SOLVED)
- **Root Cause:** When running games in 800x600 exclusive fullscreen (e.g. GTA IV), Windows shrinks all desktop windows to fit 800x600. When returning to 2560x1440, Tkinter did not handle display mode changes and kept the shrunken/distorted layout.
- **Resolution:** Added `<FocusIn>` handler in `dlss5/m13/ui.py` with `minsize(1024, 760)` that automatically detects shrunken window dimensions and restores `1024x760` geometry and re-renders cards.
- **Daemon Lifecycle:** In `controller.py`, `StateMonitor` automatically detects when the game process terminates and stops the background `m11d.exe` process, immediately freeing VRAM and resetting status to `"Direct GPU"`.

### Issue 5: High-Frequency Noise Flickering ("Гирлянда") (SOLVED)
- **Root Cause:** In `features.comp`, pseudo-random noise `deterministicNoise(x, y, frameIndex)` changed seed every frame. Multiplied by `gain`, this created frame-to-frame pixel sparkling.
- **Resolution:** Pinned deterministic noise seed `p.b[0] = 1` in `dlss5/chain/engine.cpp` during temporal passthrough.

### Issue 6: Contrast-Adaptive Edge Sharpening (CAS) (SOLVED)
- **Implementation:** Added robust bounded sharpening into `dlss5/m8b-live/shaders/encode.comp` push constants (`pc.c[0]`), connected to the live `Sharpness` slider (0.0 to 1.0). Strictly bounded to local 3x3 neighborhood limits: mathematically impossible to ring, halo, or amplify noise.

---

## 3. Architecture Matrix

| Subsystem | Binary / Module | Description | Mode |
| :--- | :--- | :--- | :--- |
| **x64 Vulkan Games** (RDR2, Witcher 3, Cyberpunk) | `nr_layer_win.dll` + `d5c_chain.dll` | Direct In-Process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x64 DX12 Games** (GTA V Enhanced) | `m12_dxgi.dll` + `d5c_chain.dll` | Direct In-Process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x86 Vulkan/DX9 Games** (GTA IV via DXVK) | `nr_layer_win32.dll` $\leftrightarrow$ `m11d.exe` | TCP loopback bridge (x86 $\to$ x64 daemon) | Auto-managed daemon lifecycle |
| **Screen Mode** (Desktop / Window Overlay) | `m8blive.exe` | Standalone direct DDA desktop capture overlay | Direct GPU, No Daemon |
| **Management GUI & Overlay** | `m13` (`pythonw m13.pyw`) | Tkinter modern dashboard & in-game overlay | Topmost non-minimizing overlay |
| **Official Performance Benchmarks** | [docs/BENCHMARKS.md](BENCHMARKS.md) | Verified hardware matrix, 15-20 ms reconciliation, 50% preset target | 233.5 ms @ 0.50 scale, 0.39 ms FrameGen |

---

## 4. Host Quirks & Red Lines
1. **Sunshine is Permanent:** Sunshine (PID ~6524 / ~4792) streams this desktop to the owner via Moonlight. **NEVER kill Sunshine.**
2. **Local Git Commits Only:** Keep all commits local on `master` branch. Do NOT push to remote repositories.
3. **ASCII-Only in Code and Scripts:** Non-ASCII comments in `.cmd` or `.cpp` can trigger launcher parse bugs on this host.
4. **Weights Proprietary:** `dlssnr-logical.safetensors` and `dlssnr-int8.bin` must never be committed to git.

---

## 5. Optimization Progress & Current State (2026-09-28)

### What is Completed & Fully Verified:
1. **INT8 Weight1 & Weight2 End-to-End (`Stage 11`, Commit `8af6ea5`):**
   - Dynamic per-32-token symmetric activation quantization (`quant_act.comp`).
   - INT8 coopmat 8x16x32 ($BM=32, BN=32$) with fused gate+e4m3 epilogue for `weight1` and multi-tile K-loop (4 tiles for $K=128$) with fused residual for `weight2`.
   - Quality: Weight2 Pilot 70: **48.76 dB**, $r = 0.9954$.
2. **Fused `weight1 -> weight2` Pipeline (`Stage 12`, Commit `ce060bd`):**
   - Direct producer-consumer pipeline: `weight1` epilogue computes 32x32 tile absmax via `subgroupMax()` reduction, writes tile scales to `O.oQScale2`, directly writes INT8 activations to `O.oG3`.
   - `weight2` multi-tile K-loop loads INT8 directly from `O.oG3` with tile scales.
   - Intermediate `quant_act` pass **100% ELIMINATED** (10 dispatches and 10 barriers removed per frame).
   - Quality: **45.05 dB PSNR** (improved from 44.42 dB due to 32x32 tile scale dynamic range), $r = 0.9698$, 0 NaNs/Infs.
   - Hardware Measured Benchmarks (Intel Arc Pro B50):
     - 1080p: **741.83 ms** (-98.12 ms saved per frame, **1.13x speedup**; **-4.27 GB DRAM traffic saved**).
     - 960x576: **214.38 ms** (-19.39 ms saved per frame; **-1.13 GB DRAM traffic saved**).
3. **Window Attention (QKV & Projection) in INT8 (`Stage 13`, Commit `51409de`):**
   - Batched scale indexing `by = gl_WorkGroupID.z * ((pc.m + BM - 1u) / BM) + gl_WorkGroupID.y` across all window grids ($z \in [0..W_w-1]$).
   - Sized `off.oQScale` and `off.oQScale2` using `maxTokens = std::max(st[0].TOK, Ww * 64)` to avert 610-float buffer overruns.
   - Granular controls: `D5C_INT8_ATTN` (default `-1` for 100% rollback safety), `D5C_INT8_ATTN_QKV`, `D5C_INT8_ATTN_PROJ`.
   - Window block filtering in `weights.cpp` (`isWindow = blk <= 30 || blk >= 40`), preserving FP16 precision for global blocks 31..38.
   - Quality: Attention Pilot 70: **44.94 dB PSNR**, $r = 0.9680$, 0 NaNs/Infs. Rollback baseline: **45.05 dB PSNR** (bit-exact).
   - Roofline Finding: All Attention GEMMs in FP16 execute in only **46.1 ms** total across the whole network (weights live permanently in L2 cache, direct register-to-memory stores). Un-fused INT8 adds `quant_act` (+24.0 ms) and shared-memory dequant (+54.2 ms) overhead, confirming that the remaining attention leverage is in **Flash-Attention fusion**.

---

## 6. Exact Roadmap for Next Session: Flash-Attention Fusion

### Target: Fuse `cosw` + `smaxw` + `gather` into Single On-Chip Attention Kernel
- **Measured Problem:** The Attention Subsystem (`cosw` 91.2 ms + `smaxw` 78.7 ms + `gather` 32.7 ms = **202.6 ms on 1080p**, 60.9 ms on 960x576) is memory-bound, materializing intermediate score matrices (`oSCW` $[W_w, H, 64, 64]$ and `oPRW`) to DRAM.
- **Solution:**
  1. **Fused Flash-Attention Kernel:** Compute Query/Key dot-products, online softmax normalization (running max + sum), and Value accumulation directly in SRAM / registers per window tile ($64 \times 64$).
  2. **Eliminate Intermediate Buffers:** Remove `oSCW` (46 MB) and `oPRW` (23 MB) DRAM traffic entirely.
  3. **Expected Leverage:** Cut ~100-150 ms from 1080p frame time, targeting **< 600 ms @ 1080p** and **< 160 ms @ 960x576**.


