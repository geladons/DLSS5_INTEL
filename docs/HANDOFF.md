# HANDOFF: DLSS 5 on Intel Arc Pro B50 (Updated 2026-09-29, v0.3 Post-Release & Game Hardening)

## 1. Quick Start & Verification in New Session
- **Production Bundle (User-Ready):**
  - Path: `C:\Users\<owner>\Desktop\production`
  - Launcher: Run `DLSS5 Manager.vbs` (zero console flash) or `DLSS 5 Manager.cmd`.
  - Content: 113 binaries and assets verified with SHA-1 hashes; INT8 weights (`dlssnr-int8.bin`, 158 MB) packaged alongside FP16 safetensors (`dlssnr-logical.safetensors`, 291 MB); x64/x86 layer manifests patched with absolute paths.
- **Port Assignment:**
  - `48990` is the canonical DLSSNR daemon TCP port (moved from 47990 to avoid collision with Sunshine Moonlight host).
- **Run Bundle Selftest:**
  ```cmd
  cd /d C:\Users\<owner>\Desktop\production\runtime\m11d
  m11d.exe --weights "C:\Users\<owner>\Desktop\production\weights\dlssnr-logical.safetensors" --selftest "C:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp"
  ```
  Must output `[selftest] processed in ... ms` and `EXITCODE=0`.
- **Verify Numerical Accuracy:**
  ```cmd
  python tools\compare_prod_accuracy.py
  ```
  Must report:
  - `vs FP16 Reference: PSNR = 45.05 dB, exact = 51.26%, max_diff = 25.0`
  - `vs Stage 14/15 Fused Reference: PSNR = 999.00 dB, exact = 100.00%, max_diff = 0.0`
- **Verify Production Manager Imports:**
  ```cmd
  python tools\test_m13_imports.py
  ```
  Must report all 18 package modules `[OK]`.
- **Verify DLL Exports:**
  ```cmd
  python tools\check_pe_exports_pure.py
  ```
  Must report all required exports for `nr_layer_win.dll`, `nr_layer_win32.dll`, `m12_dxgi.dll`, and `d5c_chain.dll`.

---

## 2. Solved Issues & Completed Milestones in Current Session

### Stage 20: Restore Public Image Baseline & Eliminate Stale C ABI Copies (2026-09-29)
1. **Image Regression:** Compared current knobs with `origin/master`: public feature conditioning was `(style=0, tone=1.0, structure=1.0, skin=-1, auto=-1)` and compose blend was `1.0`; the user's saved profile had intensity `1.65`, tone `0.85`, structure `1.6`. Reset global Phase30 defaults/profile to the public baseline; saved the previous config as `%LOCALAPPDATA%\DLSS5Manager\config.json.pre_public_baseline`. Render scale, game list, and per-game overrides were left intact.
2. **Parameter Routing:** Phase30 intensity is now distinct from legacy residual gain; the manager passes residual gain as neutral `1.0` while intensity controls compose mix. The daemon receives all Phase30 values explicitly.
3. **Overlay Photo Mode:** `overlay_focus=False` no longer forces foreground activation. Opening still sets the freeze flag; the presentation layer retains and reprocesses its original raw frame on knob changes. Foreground focus remains opt-in.
4. **Stale DLL Prevention:** The packager and x64/DX12 build scripts prefer the current `m11d/build/chain/Release/d5c_chain.dll`, avoiding stale staged copies. Full m11d Release and x64 layer builds completed; source/staged DLL hashes match.
5. **Verification & Limits:** M13 Stage 5 and Phase30/cache suites pass. In this VS Code session the detached GPU selftest stops at the 4.87 GB host-weight staging allocation without a completion/error record; the existing production selftest log from 00:11 completed successfully. New pixel parity and live GTA IV quality remain unverified. The x86 daemon remains synchronous per present (recorded 60-110 ms at 800x600) and currently has no interpolation on its TCP path; restoring image quality does not solve that separate FPS limit.

### Stage 19: GTA IV Menu Processing, Overlay Access & Game Handoff (2026-09-29)
1. **GTA IV Static Menu Processing:** The old 85% held-still bypass returned the native frame unchanged, so static menus never received DLSSNR. Static screens now get one neural pass, identical inputs reuse the processed result, and moving scenes retain the UI mask for held pixels.
2. **In-Game Controls:** The manager header no longer duplicates gain, blend, and sharpness controls already owned by Settings. The `CTRL+ALT+G` panel has a scrollable body and explicitly takes foreground focus when opened so all Phase30 controls can be reached and clicked.
3. **Game-to-Game VRAM Handoff:** x64 launches stop an x86 daemon still in its exit grace period. When the last managed game exits, DX9/DX12 pause and overlay freeze/reprocess flags return to defaults. Phase30 configuration is included in manager-launched game environments.
4. **Runtime Refresh & Verification:** Rebuilt both x64 and x86 Vulkan layers; deploy refreshes stale game-local DLLs by comparing hashes while preserving the original in `.m13bak`. `python dlss5\m13\test_stage5.py` passes. Owner-side GTA IV image-quality and input validation remains outstanding.

### Stage 17: Multi-Engine Game Presentation Fixes & Stability Hardening (Commits `d90e680`, `6505874`, `4063eaf`, `298666a`)
1. **GTA IV (x86 DX9 / DXVK) Freeze & Flickering:**
   - *Freeze Cause:* Port collision on `47990` with Sunshine (Moonlight host, PID ~7960). `nr_layer_win32.dll` connected to Sunshine instead of `m11d`, blocking synchronously for 60 seconds inside `vkQueuePresentKHR`.
   - *Flicker Cause:* Removed 85% static threshold in `nr_layer_win.c` forced constant neural passes on static menus, fluttering threshold (`abs(diff) > 6`) across D3D9 backbuffers.
   - *Fixes:* Moved daemon & layer to port `48990`; reduced socket timeout `tv` to `1500` ms (fail-fast); increased `settled()` threshold to `12`; restored 85% static menu bypass returning untouched backbuffer at clean 60 FPS; added 10-tick grace period and `gtaiv.exe` process checks before auto-stopping daemon.
2. **Red Dead Redemption 2 (x64 Vulkan) Crash on Launch:**
   - *Cause:* `nr_CreateDevice` called `next_instance(NULL, "vkCreateDevice")` with `NULL`. With RDR2 + Rockstar Social Club + Steam overlays, `vkGetInstanceProcAddr(NULL, ...)` returned a NULL pointer, causing an Access Violation `0xC0000005`.
   - *Fixes:* In `nr_layer_win.c`, lookup uses `layer_instance` first, then `next_instance_proc`, with explicit null checks and logging; intercepted `vkDestroyInstance` to reset dangling instance handles; added compute instance bypass in `nr_CreateInstance`; implemented score-based device selection in `pickDevice()` (`vk_context.cpp`) prioritizing discrete Intel Arc GPUs with `VK_KHR_cooperative_matrix` (+180 score).
3. **Metro Exodus (DirectX 12) Launch Crash:**
   - *Cause:* `gamelaunch.py` set `ENABLE_NR_LAYER=1` even for DX12 mode, causing Vulkan layer injection into the game's in-process `d5c_chain.dll` compute context. Additionally, Metro Exodus queried `IDXGIFactory2/4/6` via `QueryInterface`, bypassing unhooked vtables.
   - *Fixes:* For `mode == "dx12"`, set `ENABLE_NR_LAYER = "0"` in `gamelaunch.py`; implemented dynamic per-entry vtable patching (`patch_entry`) on `QueryInterface` to hook all derived factories (`IDXGIFactory1..7`) in `m12_hook.cpp`; added SEH `__try / __except` wrappers `safe_process_frame` in `m12_dx12.cpp` around in-process compute calls; updated port to `48990`.
4. **The Witcher 3: Wild Hunt (Next-Gen) Launcher Loop & Processing:**
   - *Cause:* Next-Gen update has two binaries: `bin\x64\witcher3.exe` (DX11) and `bin\x64_dx12\witcher3.exe` (DX12, default in `launcher-configuration.json`). Scanner picked DX11 and wrapped with `REDprelauncher.exe`; deployer only copied to `bin\x64`. Clicking "Play" in launcher launched `bin\x64_dx12\witcher3.exe` without proxy DLLs.
   - *Fixes:* In `gamescan.py`, prioritized DX12 binaries over DX11 and set `launch_exe = main.exe` when `REDprelauncher.exe` is detected to bypass the launcher GUI; in `deploy.py`, added dual sister-directory sync (`bin\x64` + `bin\x64_dx12`) deploying appropriate proxy DLLs to both directories.

### Stage 18: NVIDIA Phase30 Vendor Tuning, Games List Caching & UI Modernization (Commit `092b35f`)
1. **Games List Caching & Instant Startup:**
   - Created modular caching engine `dlss5/m13/gamescache.py` saving state to `%LOCALAPPDATA%\DLSS5Manager\games_cache.json`.
   - On manager startup, cached games load **instantly with 0 delay**, skipping drive scanning.
   - Added a visible **"Rescan Games" / "Пересканировать игры"** button in the Games tab header.
2. **Fixed Overlay "Daemon Crashed" False Alarm:**
   - Overhauled status reporting in `dlss5/m13/overlay.py`. For direct in-process games (x64 Vulkan/DX12), reports `Direct In-Process (<mode>, 0 ms lag)` in green. Reports `Daemon Active (x86)` only when `m11d.exe` is serving x86 games, and `Standby` when idle.
3. **NVIDIA Phase30 Vendor Tuning Controls Overhaul:**
   - Created `dlss5/m13/phase30.py` implementing the 7 vendor parameters:
     1. `intensity`: 0.0 .. 2.0 (default **1.6** – preserving baseline demo screenshot gain).
     2. `local_tone`: 0.0 .. 2.0 (default 1.0).
     3. `local_structure`: 0.0 .. 2.0 (default 1.5).
     4. `skin_structure`: -1.0 .. 2.0 (default 2.0; -1.0 = follow local structure).
     5. `auto_mask`: bool (default True).
     6. `style_index`: 0, 1, 2 (0: Default, 1: Natural, 2: Cinematic).
     7. `color_strength`: 0.0 .. 2.0 (default 1.0).
   - Added "Reset to Defaults" button in both Manager Settings and `CTRL+ALT+G` In-Game Overlay.
   - Full Russian and English translations in `dlss5/m13/i18n.py`.
   - Config persistence in `dlss5/m13/config.py` supporting global and per-game overrides.
   - Runtime propagation via environment variables, live JSON knobs (`%TEMP%\m13_phase30_knobs.json`), and TCP daemon commands (6..11).
4. **Rebuilt Production Bundle:**
   - Updated `_build_production.py` and regenerated bundle at `C:\Users\<owner>\Desktop\production` with all new modules and recompiled binaries.

---

### 3. Architecture & Binary Matrix

| Subsystem | Binary / Module | Description | Mode |
| :--- | :--- | :--- | :--- |
| **x64 Vulkan Games** (RDR2, Witcher 3, Cyberpunk) | `nr_layer_win.dll` + `d5c_chain.dll` | Direct in-process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x64 DX12 Games** (GTA V Enhanced, Metro Exodus) | `m12_dxgi.dll` + `d5c_chain.dll` | Direct in-process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x86 Vulkan/DX9 Games** (GTA IV via DXVK) | `nr_layer_win32.dll` $\leftrightarrow$ `m11d.exe` | TCP loopback bridge on port 48990 | Auto-managed daemon lifecycle |
| **Screen Mode** (Desktop / Window Overlay) | `m8blive.exe` | Standalone direct DDA desktop capture overlay | Direct GPU, No Daemon |
| **Management GUI & Overlay** | `m13` (`pythonw m13.pyw`) | Modern dashboard, Phase30 panel & overlay | Topmost non-minimizing overlay |
| **Official Performance Benchmarks** | [docs/BENCHMARKS.md](BENCHMARKS.md) | Verified hardware matrix | 210.2 ms @ 0.50 scale, 547 ms @ 1080p |

---

### 4. Ground-Truth Hardware Performance Matrix (Intel Arc Pro B50)

| Resolution / Extent | v0.2 Release | Baseline (Stage 13) | v0.3 Fused Attention | v0.3 Final Release | Delta from v0.2 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **1080p Native (Selftest)** | 867.2 ms | 741.8 ms | 622.9 ms | **547.0 ms** | **-320.2 ms (-36.9%)** |
| **1080p Native (Steady State)** | 909.3 ms | 839.9 ms | 904.6 ms | **767.5 ms** | **-141.8 ms (-15.6%)** |
| **960x576 (0.50x Steady State)** | 233.5 ms | 214.4 ms | 247.3 ms (cold) | **210.2 ms** | **-23.3 ms (-10.0%)** |
| **DRAM Traffic @ 1080p** | 72.5 GB | 68.3 GB | 53.1 GB | **53.1 GB** | **-19.4 GB (-26.8%)** |
| **Dispatches @ 1080p** | 1271 | 1261 | 1007 | **1007** | **-264 dispatches** |
| **FrameGen (Optical Flow)** | 0.39 ms | 0.39 ms | 0.39 ms | **0.39 ms** | **2500+ FPS** |

---

### 5. Host Quirks, Rules & Red Lines (Mandatory)
1. **Sunshine is Permanent:** Sunshine (PID ~7784 / ~7960) streams this desktop to the owner via Moonlight. **NEVER stop or kill Sunshine.**
2. **Local Git Commits Only:** Keep commits local on `master`. Do NOT push to remote repositories. Use `git add -f` for docs/*.md, *.log.
3. **ASCII-Only in Code and Scripts:** Non-ASCII comments in `.cmd` or `.cpp` can trigger launcher parse bugs on this host.
4. **Weights Proprietary:** `dlssnr-logical.safetensors` and `dlssnr-int8.bin` must NEVER be committed to git.
5. **No Foreground Long Commands:** Always launch heavy/looping commands detached or via asynchronous scripts.
6. **Port 48990:** Never revert DLSSNR daemon to port 47990 (collides with Sunshine).

---

### 6. Roadmap & Next Steps for v0.4
1. **Owner Live-Testing:**
   - Test GTA IV (verify 60 FPS in menu, no freezing, clean transition to in-game).
   - Test RDR2 (verify stable launch with Vulkan layer and in-game overlay).
   - Test Metro Exodus (verify stable DX12 launch and proxy presentation).
   - Test Witcher 3 (verify direct game launch into `bin\x64_dx12` with DLSS processing).
   - Test Phase30 panel sliders in Manager Settings and `CTRL+ALT+G` overlay.
2. **Window Transpose & Partition Vectorization:**
   - Vectorize `trans.comp` (29.6 ms @ 1080p) and `part.comp` (27.3 ms @ 1080p) to 128-bit `vec4`/`f16vec4`.
3. **Quantization of Global Blocks (Blocks 31..38):**
   - Evaluate INT8 quantization with global token normalization.
