# Weights provenance — public note

The canonical artifact this project consumes is
`dlssnr-logical.safetensors` — Safetensors, layout `dlssnr-logical-v18`,
649 tensors (579 F16 + 70 F32), 71 top-level block groups, 291,576,650 bytes,
SHA-256 `203b0af3be94078cfd17a71adc4628cffd960a4d435e4820fe994ddd9493a6a5`.

It is derived from NVIDIA's proprietary DLSS neural-rendering runtime data
(resource section, parsed by a byte-level extractor; the NVIDIA binaries are
never loaded or executed by this project). NVIDIA proprietary model data —
**not redistributed, not published, not committed to git** (`.gitignore`
covers `*.safetensors`; `work/` and `reference/` are excluded as well).

The public format specification is in
[weights-format.md](weights-format.md); the full tensor inventory is in
[weights-inventory.txt](weights-inventory.txt).

The detailed local extraction record (chain of custody, tool revisions,
hashes of intermediate artifacts) is kept **outside the repository** as a
private lab note.
