# Weights provenance — dlssnr-logical.safetensors

Date: 2026-09-20. Project: DLSS5_INTEL. LOCAL RESEARCH USE ONLY — NVIDIA
proprietary model data; extracted copy must NOT be redistributed, published,
or committed to git.

## Chain of custody

1. **Source package**: `streamline.zip` (137 MB) from the GitHub release of
   `yumlevi/renodx-dlss-installer` — https://github.com/yumlevi/renodx-dlss-installer/releases/download/latest/streamline.zip
   (community distribution package; discussed in its issue #1).
2. **DLL**: `nvngx_dlssnr.dll`, 165,840,496 bytes, FileVersion 310.8.0.0.
   - SHA-256: `ceb6432f6fbdf44d886014bcd47241932bf8b67439feef9bbdd0961436662650`
   - This byte-exact hash is the single build pinned as "supported" by the
     MLX-DLSS extractor (in TWO places: `python/mlxdlss/tools/cli.py` and
     `docs/research/2026-08-31-dlssnr-first-frame-preprocessor.md`).
   - Context: this is the DLSS 5 neural-rendering runtime leaked via the
     NBA 2K27 PC early-access build (2026-08-26, NVIDIA-signed 2026-08-11).
     The leaked build's Authenticode does not validate (documented by
     kayle2203/dlssnr-signature-repair, whose *repaired* build has a
     different hash and is NOT supported by the extractor). We never load
     or execute the DLL — the extractor only parses its bytes.
3. **Extraction tools**: `iamwavecut/MLX-DLSS` @ `06a3e11a8b68817127406ace5c764463543f699b`
   (pinned by reference/dlss-nr-on-intel README).
   - Vetted 2026-09-20: `extract_dlssnr_weights.py` fully read — pure PE
     resource parser (WEIGHTS_HT RCDATA) + safetensors writer; no network,
     no exec/eval, no env access. `unpack_dlssnr_weights.py` pattern-scanned
     clean (no requests/urllib/socket/subprocess/ctypes/base64/pickle).
4. **Commands** (run with Python 3.12.10, PYTHONPATH=work/mlx-dlss/python):
   - `extract_dlssnr_weights.py work/dll/nvngx_dlssnr.dll work/mlxw/dlssnr-packed.safetensors`
     → 153 packed tensors, payload 147,683,778 bytes, resource SHA-256 `836f445d06ec…548f4`.
   - `unpack_dlssnr_weights.py work/mlxw/dlssnr-packed.safetensors work/mlxw/dlssnr-logical.safetensors`
     → format `dlssnr-logical-v18`, 153/153 decoded, **649 tensors**, 0 unsupported.
5. **Output**: `work/mlxw/dlssnr-logical.safetensors`
   - SHA-256: `203b0af3be94078cfd17a71adc4628cffd960a4d435e4820fe994ddd9493a6a5`
   - 291.5 MB: 579 F16 + 70 F32 tensors; 71 top-level block groups
     (block0..block70) — the ViT-style DLSSNR transformer stack.
   - Inventory: docs/weights-inventory.txt.

## Notes

- The packed intermediate's file hash (f6f976ae…) differs from the hash in
  cli.py (08a39bcd…) — different tool-revision metadata in the safetensors
  header; the logical output is the canonical artifact the reference
  pipeline consumes.
- Reference repo (dlss-nr-on-intel) consumes exactly this
  `dlssnr-logical.safetensors` format.
- Git policy: `work/`, `*.safetensors`, `*.dll` are gitignored; only this
  document and the inventory are committed.
