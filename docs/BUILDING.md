# Building from source

This guide covers everything needed to compile the project from a fresh
clone and assemble the same release bundle that ships on the
[Releases page](https://github.com/geladons/DLSS5_INTEL/releases).
For the day-to-day dev loop and the validation rig, see
[CONTRIBUTING.md](../CONTRIBUTING.md) and
[docs/ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Toolchain

| Component | Required | Notes |
|---|---|---|
| Windows 10 / 11 | ✅ | tested on Windows 11 |
| [Vulkan SDK](https://vulkan.lunarg.com/) 1.4+ | ✅ | must include `glslangValidator` (shader compilation is a CMake step) |
| [Visual Studio Build Tools 2019+](https://visualstudio.microsoft.com/visual-cpp-build-tools/) | ✅ | MSVC x64/x86 compilers + vcvars; CMake generator |
| [CMake](https://cmake.org/download/) 3.25+ | ✅ | 4.x works |
| Python 3.10+ | ✅ | manager app (stdlib only), build helpers |
| PyTorch + NumPy | for validation only | needed to reproduce the golden reference dumps |

No NVIDIA SDK, no CUDA, no proprietary blobs anywhere in the build.

Clone and you are ready — every path in the build scripts is anchored
relative to the script itself (`%~dp0` / `$PSScriptRoot` /
`Path(__file__)`), so the tree can live anywhere.

## 2. Building the modules

Each native module has its own `build.cmd` (VS generator with an
automatic NMake+vcvars64 fallback — the fallback is **normal**, not an
error; one of the dev hosts has broken VS COM registration and the
scripts cope with it). Check `<module>\_build.log` after each build.

From the repo root, in any `cmd` prompt:

```cmd
cd dlss5\m8b-live   && build.cmd    :: screen/window mode app (m8blive.exe)
cd ..\m11d          && build.cmd    :: frame daemon (m11d.exe, TCP 47990)
cd ..\m11-layer     && build.cmd    :: Vulkan implicit layer, x64 + x86
cd ..\m12-dxgi      && build.cmd    :: DX12 dxgi proxy
cd ..\m6-weights-loader && build.cmd :: weights loader/validator (optional tool)
```

Artifacts land in `<module>\build\Release\` (or `build-nmake\Release\`
when the NMake fallback was used). These directories are gitignored.

The manager (`dlss5\m13`) is pure Python — nothing to compile. To assemble
a runnable production folder (manager + all runtime artifacts + weights,
with the layer manifests patched to the destination's absolute path —
the x86 Vulkan loader requires it):

```cmd
python dlss5\m13\_build_production.py [dest_dir]
:: default dest: %USERPROFILE%\Desktop\production
```

That folder is for **local runs**. The publishable bundle (no weights,
portable relative paths) is produced from it by
`release\assemble-release.cmd` in section 4.

### DXVK for the DX9/10/11 path

DXVK binaries are **not** built here and not vendored; they are fetched
by the helper (downloads the official release, x64 + x86):

```cmd
dlss5\m11-layer\_get_dxvk.cmd
```

## 3. Running what you built (quick smoke)

- **Screen/window mode:** `tools\RUN-DEMO.cmd` (needs the weights —
  see below; first frame can take tens of seconds while weights upload).
- **Daemon selftest against the torch goldens:**
  `dlss5\m11d\build\Release\m11d.exe --selftest <frame.bmp>`
  (compare `out\live_featV.bin` / `live_head.bin` — see
  [docs/ARCHITECTURE.md](ARCHITECTURE.md#validation-rig)).
- **Vulkan path end-to-end:** register the layer with
  `dlss5\m11-layer\_register.cmd`, run a Vulkan app with
  `ENABLE_NR_LAYER=1`, daemon on `127.0.0.1:47990`.

## 4. Assembling the release bundle

The bundle is a plain folder (manager + runtime + empty `weights\`).
It is **not committed to git** — assemble it from a production folder
(that is simply a folder where you copied the release-relevant outputs):

```cmd
release\assemble-release.cmd [source-production-dir] [target-dir]
:: defaults: %USERPROFILE%\Desktop\production  ->  release\out\DLSS5-Demo-Bundle
```

The script copies the manager, `runtime\` (layer, m11d, m12, m8b, dxvk)
and writes a placeholder into `weights\` explaining what file the user
must supply. Weights (`*.safetensors`), logs and frame dumps are always
excluded — see [docs/weights-format.md](weights-format.md).

Then zip the folder and attach it to a GitHub Release
(see [release/README.md](../release/README.md#publishing-checklist)).

## 5. The weights (needed to run anything)

Not in the repo, not in the bundle — NVIDIA proprietary data. One file:
`dlssnr-logical.safetensors` (~291.5 MB, layout `dlssnr-logical-v18`,
649 tensors). Full spec and SHA-256:
[docs/weights-format.md](weights-format.md). The loader also accepts any
path via `--weights`, and `m6` searches `work\mlxw\` relative to the
working directory and upwards from the executable.

## 6. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `build.cmd` reports the VS generator fallback | Normal on hosts with broken VS COM registration; NMake+vcvars64 is used instead. |
| `cmake ... instance is not known` | VS Setup.Configuration discovery failed; the fallback handles it. Reinstall VS Build Tools if even vcvars is missing. |
| Vulkan app does not get processed | Layer registration (`_register.cmd`) + `ENABLE_NR_LAYER=1` + daemon running on port 47990. |
| Daemon complains about weights | File name/location — see section 5. |
| Shader compile errors at CMake time | Old Vulkan SDK — upgrade to 1.4+. |
| Everything builds, first frame takes forever | Not a hang: ~30 s weights upload, up to a minute for the first frame at high resolution. |
