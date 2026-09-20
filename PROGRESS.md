# DLSS5_INTEL — Progress Log

Project: Intel Arc port of DLSS 5-style neural rendering, whole-desktop
(NeuralScreen analog). Root: `C:\Users\AI\Desktop\DLSS5_INTEL`

## Done (2026-09-19)

- [x] Workspace: `reference/` (5 cloned repos, read-only) + `dlss5/` + README.md
- [x] git repo initialized (initial commit `26494de`; `reference/` gitignored)
- [x] Dev environment inventory: **Arc Pro B500 (Battlemage) + B50**, 32 GB,
      Win11 Pro WS, Git 2.55, Node 24, .NET, VS Build Tools 2019;
      missing Python/VulkanSDK/CMake/Rust
- [x] `win-desktop-control` skill (local, vetted) — screenshots, windows,
      UIA read/elements, clicks, keys (with documented host quirks)

## In progress

- [ ] Deep analysis of `reference/dlss-nr-on-intel` (Linux Vulkan layer →
      Windows port path, XMX/cooperative_matrix requirements)
- [ ] Deep analysis of `reference/NeuralScreen` (capture → inference →
      present pipeline, overlay architecture, app/window modes)

## Toolchain (2026-09-19)

- Vulkan SDK 1.4.357.0 (vulkaninfoSDK.exe at C:\VulkanSDK\1.4.357.0\Bin\)
- CMake 4.4.3 (C:\Program Files\CMake\bin) — not on stale PATH of running processes, use full path
- Python 3.12.10 (%LOCALAPPDATA%\Programs\Python\Python312) — same PATH caveat
- VS Build Tools 2019 with VC.Tools.x86.x64 (cl.exe available)
- ninja: not found (use VS generator / MSBuild; non-blocking)
- **GPU reality check:** the PCI GPU is **Intel Arc Pro B50** (VEN_8086&DEV_E212,
  driver 101.8805); the "Arc Pro B500" entry is a root-enumerated virtual display
  (ROOT\DISPLAY\0000, driver 11.30.4.434) — not a real Vulkan device.
- **VK_KHR_cooperative_matrix: SUPPORTED (rev 2) on the B50**, compute stage only,
  apiVersion 1.4.348. Full report: docs\vulkaninfo-gpu.txt — M0 gate PASSED.
- UAC: ConsentPromptBehaviorAdmin=0 (elevate w/o prompts), SmartScreen off (owner-approved fix).

## Next

- [ ] Toolchain install (Vulkan SDK, CMake, Python; VS Build Tools C++ workload check)
- [ ] Decide project stack (language, capture API: DXGI Desktop Duplication vs
      Vulkan layer; inference: DirectML / Vulkan compute / OpenVINO?)
- [ ] Milestone 1: capture a frame from a window on Arc + run any NN pass on it
- [ ] Vulkan on Arc B500: verify `VK_KHR_cooperative_matrix` support
      (`vulkaninfo` / capsule)

## Notes

- dlss-nr-on-intel targets Linux; port focus = Windows first, keep the Vulkan
  layer design portable (layers work on Windows too).
- Never run binaries/scripts from `reference/` without code review
  (ClickFix incident 2026-09-19, see workspace memory).
- Session work style: short steps, progress written here, long analysis
  delegated to sub-agents.
## M0 result (2026-09-19) - cooperative-matrix (XMX) probe: PASS

- Built dlss5/m0-coopmat-probe/ (raw Vulkan C++17, no libs beyond vulkan-1):
  main.cpp + probe.comp (GL_KHR_cooperative_matrix) + CMakeLists.txt + build.cmd.
- Device: Intel Arc Pro B50, apiVersion 1.4.348, driver 101.8805, subgroupSize 32.
- Enumerated 4 configs at runtime via vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR:
  [0] 8x16x16 f16xf16->f32, [1] 8x16x32 u8xu8->u32, [2] 8x16x32 s8xs8->s32,
  [3] 8x16x16 f16xf16->f16 - all scope=SUBGROUP. Chosen: [0] (f16->f32).
- Numeric verify: 8x16 tile, C = A*B (GPU coopMatMulAdd, f32 result) vs CPU f32
  reference: 128/128 within 1% tolerance (max rel err 0.0000%). XMX works.
- Timing: single dispatch+sync 0.473 ms; 100-dispatch batch avg 0.0033 ms/dispatch.
- Full build+run console log: docs/m0-coopmat-probe.log.
- Blockers: none. Notes: build orchestration is build.cmd (cmd batch) because the
  exec wrapper mangles $vars even in .ps1 -File mode; batch %VAR% is immune.
  Shader shape is injected via glslangValidator -D defines matching the runtime
  chosen config (probe writes chosen.txt beside probe.exe and exits 2 on mismatch
  so build.cmd recompiles + retries, max 3 attempts).

## M1 result (2026-09-19) - DXGI Desktop Duplication capture: PASS

- Built dlss5/m1-frame-capture/ (raw DXGI 1.2/1.5 + D3D11, C++17, deps: Windows
  SDK only — dxgi.lib/d3d11.lib): main.cpp + CMakeLists.txt + build.cmd.
- Device: D3D11 HARDWARE on Intel Arc Pro B50, feature level 11.1, VRAM 16.2 GB.
  Output \\.\DISPLAY5, 2560x1440, rotation IDENTITY, desktop format B8G8R8A8_UNORM.
- Duplication: IDXGIOutput5::DuplicateOutput1 pinning B8G8R8A8 was REJECTED by the
  driver (fell back to DuplicateOutput, driver still hands B8G8R8A8 — format-flap
  pinned path to re-test in M3). Errors for locked session / busy duplicator /
  disconnected session are mapped to clear messages (untested paths, no blocker).
- Capture loop: AcquireNextFrame(500ms) -> CopySubresourceRegion to staging ->
  Map -> BGRA8 CPU buffer; DXGI_ERROR_ACCESS_LOST handled with duplication
  re-create. Run: 300 attempts, 300 acquired, 0 timeouts, 37 dirty-updates /
  263 same-texture repeats, 0 errors.
- fps: avg 69.0 (interval min 3.98 ms = 251 fps, max 85.5 ms = 11.7 fps).
- 5 snapshots out\snapshot_{60..300}.bmp (14745654 B each, 2560x1440x4+54).
  Luma variance ≈ 9745, range [0,255] over 57.6k samples — REAL desktop content,
  not a black screen; snapshot_60 != snapshot_300 (cursor moved between grabs).
- Full build+run console log: docs/m1-capture.log (gitignored, force-added).
- Blockers: none. LESSON: DDA is dirty-rect driven — an idle VM desktop yields
  zero frames (first run: 300/300 timeouts). m1dda has a net-zero cursor-wiggle
  activity generator (argv[5]=0 disables) so the compositor produces frames.
