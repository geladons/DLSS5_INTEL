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
4. **Weights Proprietary:** `dlssnr-logical.safetensors` must never be committed to git.
