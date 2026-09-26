# Contributing to DLSS5_INTEL

Thanks for your interest! This repo is both a research project and an
engineering portfolio — good-faith contributions are welcome. Before you start,
read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and skim
[DEV_STATE.md](DEV_STATE.md) (the lab notebook with measured numbers and
host quirks).

## Ground rules

1. **Never commit weights or binaries.** `*.safetensors`, `*.dll`, `*.exe`,
   `reference/`, `work/`, `build/`, `out/` are gitignored — keep it that way.
   No credentials, ever.
2. **No NVIDIA code.** Clean-room our own implementation; reference code in
   `reference/` is read-only study material and never ships.
3. **One logical change per PR**, small reviewable diffs.
4. **ASCII-only** in code comments, identifiers and `.cmd` files (a launcher
   host quirk turns non-ASCII bytes into garbage when executed).
5. **OOP/modular**: a new subsystem is its own translation unit
   (`foo.h`/`foo.cpp` or a Python module) with a small public interface,
   wired in at one call site. Soft cap ~600 lines per file. Do not grow
   `main.cpp` for a new concern. New file-scope globals are not allowed;
   state travels in structs/classes passed by reference.

## Building

Full guide (toolchain, every module, bundle assembly, troubleshooting):
[docs/BUILDING.md](docs/BUILDING.md). Quick version — Windows, Vulkan SDK,
CMake, VS Build Tools 2019+, then from the repo root:

```cmd
cd dlss5\m8b-live && build.cmd        :: live screen/window app
cd ..\m11d          && build.cmd      :: frame daemon
cd ..\m11-layer     && build.cmd      :: Vulkan layer (x64 + x86)
```

Each module writes `_build.log` — check it for errors. The VS-generator
discovery on the primary dev host is broken; `build.cmd` falls back to
NMake+vcvars64 automatically (that is normal there, not a bug in your tree).

## Validation rig (mandatory before merging anything touching the chain)

1. `m11d --selftest <frame.bmp>` → compare `live_featV.bin` / `live_head.bin`
   against the torch goldens: features bit-exact on 15/16 channels, head
   meandiff ≈ 0.0083.
2. `blend=0` selftest must be a bit-exact passthrough (maxdiff 0).
3. 45-frame `m8blive` PASS run with verify-frame dumps.
4. `dlss5/m13/_smoke.py` and `_ui_smoke.py` against a live daemon.

Post the numbers in the PR description — the template asks for them.

## Where things live

| Path | What |
|---|---|
| `dlss5/chain` | the model as an OOP module (VkContext/WeightsStore/ChainArena/ChainRecorder/ChainEngine) |
| `dlss5/m11-layer` | Vulkan implicit layer (x64/x86) |
| `dlss5/m11d` | TCP frame daemon + NRCT control channel |
| `dlss5/m12-dxgi` | DX12 dxgi proxy |
| `dlss5/m8b-live` | screen/window mode app |
| `dlss5/m13` | manager (Python stdlib only) |
| `docs/` | analysis, architecture, handoffs, logs |
| `AGENTS.md` / `DEV_STATE.md` | working rules + day-by-day engineering log |

## AI agents

This repository is developed with AI coding agents. `AGENTS.md` is the
operating manual for them (host quirks, red lines, run/verify loop); humans
may safely ignore it.

## Code of conduct

Be kind, be direct, cite evidence. See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
