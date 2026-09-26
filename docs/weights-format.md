# Weights format — what you need to run the demo

The DLSSNR model weights are **NVIDIA proprietary data**. This project does
**not** distribute them — not in the repository, not in release bundles, not
in issues or chats. This document specifies exactly what the loader expects,
so you can supply the file yourself.

## Required artifact

| Field | Value |
|---|---|
| File name | `dlssnr-logical.safetensors` |
| Container | [Safetensors](https://github.com/huggingface/safetensors) (little-endian, no torch pickle) |
| Layout tag | `dlssnr-logical-v18` (logical/unpacked tensor layout consumed by the reference pipeline) |
| Tensors | **649** total — 579 × F16, 70 × F32 |
| Structure | 71 top-level block groups (`block0` … `block70`) — the ViT-style DLSSNR transformer stack, plus stem/head tensors |
| File size | 291,576,650 bytes (~291.5 MB) |
| Resident VRAM | ~325 MB (after pack / fuse-fold / de-swizzle) |
| SHA-256 | `203b0af3be94078cfd17a71adc4628cffd960a4d435e4820fe994ddd9493a6a5` |

A full tensor inventory ships in [weights-inventory.txt](weights-inventory.txt).

## Where to put it

- **Release bundle:** the bundle's `weights\` folder — the manager finds it
  automatically at startup ("weights" item in the setup checklist goes green).
- **From source / daemon:** pass any path explicitly —
  `m11d.exe --weights C:\path\to\dlssnr-logical.safetensors ...`
  (the flag is **mandatory**; the daemon refuses to start without it).

## How it is loaded

`dlss5/chain/WeightsStore` (with the loader in `dlss5/m6-weights-loader`):

1. Parses the Safetensors header (pure byte parsing, no `torch` dependency).
2. Validates dtypes (F16/F32 only) and tensor count.
3. Packs, fuse-folds and de-swizzles into the resident layout expected by the
   14 chain pipelines.

Validation of any supplied file: run the daemon self-test
(`m11d.exe --selftest <frame.bmp>`) and compare `live_featV.bin` /
`live_head.bin` against the golden dumps — features must be bit-exact on
15/16 channels and head meandiff ≈ 0.0083.

## Legal note

The weights are extracted from NVIDIA's DLSS neural-rendering runtime and
remain NVIDIA's property. Do not redistribute the file; do not commit it to
git (`.gitignore` already excludes `*.safetensors` — keep it that way).
Whether you may possess or use such a file depends on your jurisdiction and
the terms you obtained it under — that is on you, not on this project.
