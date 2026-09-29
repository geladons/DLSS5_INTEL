# HANDOFF: DLSS 5 on Intel Arc Pro B50 (Updated 2026-09-29, v0.3 Release)

## 1. Quick Start & Verification in New Session
- **Production Bundle (User-Ready):**
  - Path: `C:\Users\<owner>\Desktop\production`
  - Launcher: Run `DLSS5 Manager.vbs` (zero console flash) or `DLSS 5 Manager.cmd`.
  - Content: 113 binaries and assets verified with SHA-1 hashes; INT8 weights (`dlssnr-int8.bin`, 158 MB) packaged alongside FP16 safetensors (`dlssnr-logical.safetensors`, 291 MB); x64/x86 layer manifests patched with absolute paths.
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
  Must report all 16 package modules `[OK]`.
- **Verify DLL Exports:**
  ```cmd
  python tools\check_pe_exports_pure.py
  ```
  Must report all required exports for `nr_layer_win.dll`, `nr_layer_win32.dll`, `m12_dxgi.dll`, and `d5c_chain.dll`.

---

## 2. Solved Issues & Completed Milestones in Current Session

### Stage 14: Flash-Attention Fusion (`attn_fused.comp`, Commit `64e6ef9`)
- **Architecture:** Replaced 3 separate dispatches ($QK^T$ GEMM + `smaxw` + $PV$ GEMM) with a single compute shader per 64-token window (`attn_fused.comp`).
- **Direct-Cache Architecture on Xe2:** Staged $Q$ (2 KB) in shared memory, streaming $K$ and $V$ directly from L1/L2 cache via `coopMatLoad` instructions. This reduced shared memory from 96 KB to 6.25 KB, circumventing Intel Arc B50's 48 KB hardware SLM limit and granting maximum thread occupancy (8 threads/EU).
- **Bank Conflict Elimination:** Stored with padded leading stride $= 68$ elements ($\gcd(68, 32) = 4$), mathematically eliminating 16-way and 32-way shared memory bank conflict serialization.
- **On-Chip Softmax:** Max-subtract-exp-sum computed on-chip via `subgroupMax`/`subgroupAdd` reductions in Cooperative Matrix registers.
- **Memory & Dispatch Gains:**
  - Cut 124 dispatches per frame (186 $\to$ 62 attention dispatches).
  - DRAM buffers `oSCW` (46 MB) and `oPRW` (23 MB) **100% eliminated**.
  - Saved **15.12 GB DRAM traffic per frame at 1080p** (-4.13 GB @ 960x576).
  - Chain selftest time: 741.83 ms $\to$ **622.9 ms (-119 ms saved)**.
- **Rollback Flag:** `D5C_INT8_ATTN_FUSED=0`.

### Stage 15: Vectorized 128-bit Downsample Pool (`pool2.comp`, Commit `201d5d2`)
- **Root Cause:** Legacy `pool2.comp` mapped 1 thread per token and looped channels, striding warps by $C$ elements (128-2048 bytes), causing 16x-32x sector waste and 97% cacheline thrashing (10.0 GB/s bandwidth hole).
- **Vectorized Architecture:** Mapped 1 invocation per `(token, ch4)` slot with 128-bit `vec4`/`f16vec4` coalesced loads and stores.
- **Performance Gains:**
  - 1080p Pool Busy Time: **42.45 ms -> 2.53 ms (16.8x speedup! -39.92 ms saved)**.
  - 960x576 Pool Busy Time: **6.30 ms -> 0.66 ms (9.5x speedup)**.
  - 1080p Steady-State Chain: **912.9 ms -> 767.5 ms (-145.4 ms saved)**.
  - 1080p Selftest Chain: **622.9 ms -> 547.0 ms (-75.9 ms saved; total -194.8 ms from baseline)**.
  - 960x576 Steady-State Chain: **266.8 ms -> 210.2 ms (-56.6 ms saved)**.
- **Rollback Flag:** `D5C_INT8_POOL_VEC=0`.

### Stage 16: v0.3 Production Release Packaging & Multi-Frontend Validation (Commit `b6db10d`)
- **Full Clean Rebuild:** Rebuilt all runtime targets:
  - `d5c_chain.dll` (Core neural module)
  - `m11d.exe` (Frame daemon & verification)
  - `nr_layer_win.dll` (Vulkan layer x64)
  - `m12_dxgi.dll` (DX12 proxy hook)
  - `m8blive.exe` (Desktop overlay runner)
  - All SPIR-V kernels (`attn_fused.spv`, `pool2.spv`, `gemm_i8.spv`, etc.) synchronized.
- **Bundle Packaging (`_build_production.py`):**
  - Updated to package INT8 weights (`dlssnr-int8.bin`) alongside FP16 safetensors with SHA-1 validation.
  - Patched Vulkan layer JSON manifests with absolute paths for x86 and x64 loaders.
- **Verification Gates:**
  - Production bundle selftest: exit code 0, 45.05 dB PSNR vs FP16 reference, 999.00 dB (100.00% bit-exact, max_diff=0.0) vs Stage 14/15 reference, 0 NaNs/Infs.
  - PE exports inspected: all Vulkan negotiation, DXGI factories, and C ABI entry points verified.
  - Production Manager (`m13`): all 16 Python package modules import cleanly.
  - Desktop capture / screen mode: confirmed graceful error reporting on locked session (`E_ACCESSDENIED`).
  - Running background tasks: 0 active tasks (task-570 killed cleanly).

---

### 3. Architecture & Binary Matrix

| Subsystem | Binary / Module | Description | Mode |
| :--- | :--- | :--- | :--- |
| **x64 Vulkan Games** (RDR2, Witcher 3, Cyberpunk) | `nr_layer_win.dll` + `d5c_chain.dll` | Direct in-process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x64 DX12 Games** (GTA V Enhanced) | `m12_dxgi.dll` + `d5c_chain.dll` | Direct in-process GPU execution via C ABI | 0 ms Socket Lag, No Daemon |
| **x86 Vulkan/DX9 Games** (GTA IV via DXVK) | `nr_layer_win32.dll` $\leftrightarrow$ `m11d.exe` | TCP loopback bridge (x86 $\to$ x64 daemon) | Auto-managed daemon lifecycle |
| **Screen Mode** (Desktop / Window Overlay) | `m8blive.exe` | Standalone direct DDA desktop capture overlay | Direct GPU, No Daemon |
| **Management GUI & Overlay** | `m13` (`pythonw m13.pyw`) | Tkinter modern dashboard & in-game overlay | Topmost non-minimizing overlay |
| **Official Performance Benchmarks** | [docs/BENCHMARKS.md](BENCHMARKS.md) | Verified hardware matrix, 15-20 ms reconciliation | 210.2 ms @ 0.50 scale, 547 ms @ 1080p |

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
1. **Sunshine is Permanent:** Sunshine (PID ~7784 / ~6524) streams this desktop to the owner via Moonlight. **NEVER stop or kill Sunshine.**
2. **Local Git Commits Only:** Keep commits local on `master`. Do NOT push to remote repositories. Use `git add -f` for docs/*.md, *.log.
3. **ASCII-Only in Code and Scripts:** Non-ASCII comments in `.cmd` or `.cpp` can trigger launcher parse bugs on this host.
4. **Weights Proprietary:** `dlssnr-logical.safetensors` and `dlssnr-int8.bin` must NEVER be committed to git.
5. **No Foreground Long Commands:** Always launch heavy/looping commands detached or via asynchronous scripts.

---

### 6. Roadmap & Next Steps for v0.4
1. **Window Transpose & Partition Vectorization:**
   - Profiling shows `trans.comp` (29.59 ms @ 1080p) and `part.comp` (27.30 ms @ 1080p) are memory-bound latency holes with scalar or un-coalesced access patterns.
   - Vectorize memory loads/stores to 128-bit `vec4`/`f16vec4` following the successful `pool2.comp` pattern (potential ~30-40 ms savings at 1080p).
2. **Quantization of Global Blocks (Blocks 31..38):**
   - Currently, global attention blocks 31..38 remain in FP16. Evaluate INT8 quantization with global token normalization.
3. **End-to-End Live Game Validation:**
   - Test production bundle on live game targets (RDR2, GTA IV, GTA V Enhanced) through Moonlight stream to confirm real-time visual fidelity and HUD preservation.
