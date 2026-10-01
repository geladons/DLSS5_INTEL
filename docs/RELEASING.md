# Release guide - assembling and publishing the public bundle

This document describes how the public `DLSS5-Demo-Bundle` is produced from
a source checkout, what is in it, and the checklist for attaching it to a
GitHub Release. It reflects the current scripts
(`dlss5/m13/_build_production.py`, `release/assemble-release.cmd`) - read
them if behavior here and code disagree; the scripts win.

Related docs: [BUILDING.md](BUILDING.md) for toolchain details,
[TECHNICAL.md](TECHNICAL.md) for architecture,
[weights-format.md](weights-format.md) for the model file the user must
supply, [release/README.md](../release/README.md) for the short version.

## 1. Two different artifacts - do not conflate

| Artifact | Produced by | Contains weights? | Purpose |
|---|---|---|---|
| **Production folder** | `python dlss5\m13\_build_production.py [dest]` (default `%USERPROFILE%\Desktop\production`) | **Yes** - private copy for the owner/local use | A self-contained folder the owner can run from anywhere on this machine |
| **Public bundle** | `release\assemble-release.cmd [src] [dst]` run on top of a production folder | **No** - `*.safetensors` is excluded on purpose; `weights\README.txt` placeholder is written | The zip attached to GitHub Releases for users |

`_build_production.py` stages the private bits: the m13 manager package
(whitelisted `.py` files + `icon.ico`), runtime artifacts (`m11d.exe` +
shaders, `m8blive.exe` + shaders, x64 and x86 layer DLLs + manifests, DXVK
x32/x64 DLLs, `m12_dxgi.dll`), three copies of `d5c_chain.dll` (next to
`m11d`, the x64 layer and `m12_dxgi.dll`), the safetensors weights and the
optional `dlssnr-int8.bin`. Every copied binary is SHA-1 verified against
its source, and the Vulkan layer manifests are patched so `library_path`
is an **absolute** path into the destination (the x86 Vulkan loader rejects
relative paths with error 87 - `_build_production.py:195-208`).

`assemble-release.cmd` then robocopies the production folder into
`release\out\DLSS5-Demo-Bundle` excluding weights, logs,
`beforeafter\` sources, `__pycache__` and runtime `out\` frame dumps, and
writes the `weights\README.txt` placeholder telling the user exactly which
file to drop in (`assemble-release.cmd:31-49`).

**Policy:** the public bundle and the production folder must never be
committed to git (`release/out/` and `*.safetensors` are gitignored; the
production folder lives outside the repo). Never attach or link weights to
a release.

## 2. Prerequisites

1. A clean build of every native module - from the repo root, in `cmd`:

   ```cmd
   cd dlss5\m8b-live  && build.cmd
   cd ..\m11d         && build.cmd
   cd ..\m11-layer    && build.cmd
   cd ..\m11-layer    && _build_x86.cmd    :: 32-bit layer for x86 games
   cd ..\m12-dxgi     && build.cmd
   ```

   `build.cmd` uses the VS generator and falls back to NMake+vcvars64 on
   hosts where VS instance discovery is broken - the fallback is normal,
   check each `_build.log` anyway. Artifacts land in
   `<module>\build\Release\` (or `build-nmake\`); the x86 layer lands in
   `dlss5\m11-layer\x86\`.

2. Python 3.10+ (the manager and both assembly scripts are stdlib-only).

3. The weights file at `work\mlxw\dlssnr-logical.safetensors` - required by
   `_build_production.py` for the *private* folder (it refuses to run
   without it). It is stripped again for the public bundle. If you only
   want the public bundle and have an existing production folder, you can
   skip step 3 and run `assemble-release.cmd` directly on it.

4. Optional: `work\mlxw\dlssnr-int8.bin` for the INT8 fused path (copied to
   the private bundle when present).

## 3. Step-by-step

```cmd
:: 1) private production folder (contains weights - stays local)
python dlss5\m13\_build_production.py
::    -> %USERPROFILE%\Desktop\production

:: 2) public bundle (weights stripped)
release\assemble-release.cmd
::    -> release\out\DLSS5-Demo-Bundle
```

`_build_production.py` writes, next to `m13\` and `runtime\`:
`DLSS5 Manager.vbs` (console-free launcher probing `pyw`/`pythonw`/standard
per-user Python installs), `M13.cmd` and `DLSS 5 Manager.cmd` (fallback
launchers) and `README.txt` (in-bundle quick start, EN + short RU).

### Bundle layout (production == public minus weights)

```
<bundle>\DLSS5 Manager.vbs        console-free launcher
<bundle>\M13.cmd                  fallback launcher
<bundle>\README.txt               quick start (EN + RU short)
<bundle>\m13\                     manager package (py + icon.ico)
<bundle>\runtime\m11d\            m11d.exe, *.spv, d5c_chain.dll
<bundle>\runtime\m8b\             m8blive.exe, *.spv
<bundle>\runtime\layer\x64\       nr_layer_win.dll + manifest + d5c_chain.dll + *.spv
<bundle>\runtime\layer\x86\       nr_layer_win32.dll + manifest
<bundle>\runtime\m12\             m12_dxgi.dll + d5c_chain.dll + *.spv
<bundle>\runtime\dxvk\x32|x64\    DXVK d3d9/d3d11/d3d10core/dxgi DLLs
<bundle>\weights\                 private: the safetensors file;
                                  public: README.txt placeholder only
<bundle>\logs\                    runtime logs (created empty)
```

## 4. Verification before publishing

Run this checklist on the assembled bundle (not the production folder):

1. `dir /s /b release\out\DLSS5-Demo-Bundle\*.safetensors` -> **no hits**.
   Same for `*.bin` under `weights\` - the INT8 pack must not ship either.
   `weights\README.txt` must exist and name `dlssnr-logical.safetensors`
   with its SHA-256.
2. Confirm both manifests carry **absolute** `library_path`s that point
   inside the bundle (`runtime\layer\x64\VkLayer_dlssnr_win.json`,
   `runtime\layer\x86\VkLayer_dlssnr_win32.json`). They were patched for
   the production dir - if the bundle will be unzipped anywhere else,
   verify the manager re-patches them on first run (it does - autosetup
   registers whatever `paths.py` resolves) or document the constraint.
   The manifests' `enable_environment` / `disable_environment` keys gate
   the layer on `ENABLE_NR_LAYER`.
3. Smoke the private production folder on the bench VM first:
   `DLSS5 Manager.vbs` -> autosetup green (weights found, layers
   registered, daemon up on **48990**) -> vkcube demo -> one real
   enable/launch roundtrip.
   Only then repackage for the public bundle and smoke the *bundle* with
   the weights dropped into `weights\` the way a user would.
4. Confirm nothing under `logs\`, `beforeafter\`, `runtime\*\out\` or
   `m13\__pycache__\` made it into the bundle.
5. Zip as `DLSS5-Demo-Bundle-<tag>.zip`. The zip root should be the bundle
   folder itself.

## 5. Publishing

1. Tag the release commit (`git tag vX.Y[.Z][-suffix]`), push the tag.
2. Attach the zip to the GitHub Release; release notes come from
   `CHANGELOG.md`.
3. If the comparison page changed, verify GitHub Pages still serves
   `docs/compare.html` (the README links to it).
4. Never attach weights, logs, or the private production folder.

## 6. Known packaging limitations (current snapshot)

- **Screen mode is not portable.** `m8blive.exe` reads weights from a
  hard-coded dev-tree path (`dlss5/m8b-live/main.cpp:1831`), so the Screen
  tab will fail on user machines until the app gains a `--weights` flag or
  a `weights\`-relative lookup. The rest of the bundle is unaffected.
- The bundle still expects the user to install Python 3.10+; the launchers
  probe the usual locations and `README.txt` says so.
- Daemon port is **48990**. Anything still printing 47990 (older docs,
  older bundles) is stale - Sunshine owns 47990 on the bench host.
- The public bundle intentionally contains no weights and no logs; bug
  reports from bundle users need the game's API, GPU/driver and the
  `logs\` folder reproduced locally.

## 7. Relationship to the source tree

The same code runs in three contexts. `dlss5/m13/paths.py` resolves
runtime artifacts production-first (`runtime\...`), then falls back to the
dev-tree layout (`dlss5\m11d\build\Release\...` etc.), so the repo, the
production folder and the public bundle share one code path. A source
checkout can be run directly (`python dlss5\m13\m13.pyw`) as long as the
native modules were built in place; weights are looked up under
`work\mlxw` in dev layout.
