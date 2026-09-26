# Release bundle

The public demo bundle is **not committed to git** (binaries, and no weights
by policy). It is assembled from the local production folder:

```cmd
release\assemble-release.cmd [source-production-dir] [target-dir]
```

- default source: `%USERPROFILE%\Desktop\production`
- default target: `release\out\DLSS5-Demo-Bundle`

The script excludes weights (`*.safetensors`), logs, the raw before/after
shots, `__pycache__` and runtime frame dumps, and writes a `weights\README.txt`
placeholder pointing to [docs/weights-format.md](../docs/weights-format.md).

## Publishing checklist

1. `release\assemble-release.cmd` → bundle folder.
2. Smoke-test the bundle on the bench VM: `DLSS5 Manager.vbs` → autosetup
   green (weights present locally) → daemon up → vkcube demo
   (`runtime\m11d` + layer) → one game enable/launch roundtrip.
3. Zip the folder as `DLSS5-Demo-Bundle-<tag>.zip` → attach to a
   [GitHub Release](../../releases) tagged `v0.1-demo`, description from
   [CHANGELOG.md](../CHANGELOG.md). Status: ✅ published (see
   [v0.1-demo](../../releases/tag/v0.1-demo)).
4. Enable GitHub Pages (`Settings → Pages → docs/`) so
   [docs/compare.html](../docs/compare.html) works as the interactive
   before/after slider linked from the README.
5. Never attach or link weights anywhere.

Full toolchain / build-from-source guide for contributors:
[docs/BUILDING.md](../docs/BUILDING.md). Quick start for the downloaded
bundle: unpack, put the weights into `weights\`, launch
`DLSS5 Manager.vbs` — details in `README.txt` inside the bundle and in
the [main README](../README.md#quick-start-release-bundle).

## What is in the bundle

| Path | Content |
|---|---|
| `DLSS5 Manager.vbs`, `M13.cmd` | console-free launcher + fallback |
| `m13\` | manager app (Python stdlib only, needs Python 3.10+) |
| `runtime\layer\` | Vulkan implicit layer, x64 + x86 |
| `runtime\m11d\` | frame daemon + shaders |
| `runtime\m12\` | DX12 dxgi proxy |
| `runtime\dxvk\` | DXVK d3d9/d3d11/dxgi/d3d10core, x64 + x86 |
| `runtime\m8b\` | screen/window mode app + shaders |
| `weights\` | **empty by design** — user supplies the file (see above) |
| `README.txt` | quick start for the bundle |
