# cmp_m9.py — compare m9-unet GPU boundary dumps vs torch golden (dump_torch.py).
import json
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("C:/Users/AI/Desktop/DLSS5_INTEL/work/_ref_test")
GPU = pathlib.Path("C:/Users/AI/Desktop/DLSS5_INTEL/dlss5/m9-unet/build/Release/out")


def e4m3(x):
    x = np.asarray(x, np.float32)
    mag = np.minimum(np.abs(x), np.float32(448.0))
    bits = mag.view(np.uint32)
    exponent = np.maximum((bits >> 23) & 0xFF, 121) - 3
    step = (exponent << 23).astype(np.uint32).view(np.float32)
    recip = ((254 - exponent) << 23).astype(np.uint32).view(np.float32)
    v = mag * recip
    fl = np.floor(v)
    diff = v - fl
    rounded = np.where(diff > 0.5, fl + 1, np.where(diff < 0.5, fl, np.where(fl % 2 == 0, fl, fl + 1)))
    out = rounded * step
    return np.where(x < 0, -out, out).astype(np.float32)


def load_gpu(name, f16):
    p = GPU / f"gpu_{name}{'.f16' if f16 else '.bin'}"
    if not p.exists():
        return None
    if f16:
        return np.fromfile(p, dtype=np.uint16).view(np.float16).astype(np.float32)
    return np.fromfile(p, dtype=np.float32)


shapes = json.loads((ROOT / "golden_shapes.json").read_text())

MAP = [
    # gpu name, golden name, f16?, golden transform
    ("w0raw", "w0", False, None),
    ("w4raw", "w4", False, None),
    ("w22raw", "w22", False, None),
    ("ds4", "ds4", True, None),
    ("ds8", "ds8", True, None),
    ("ds14", "ds14", True, None),
    ("ds22", "ds22", True, None),
    ("ss", "w30", True, None),
    ("bridge", "downsample", True, None),
    ("g31", "g31", True, None),
    ("g38", "g38", True, None),
    ("mrg39", "decoder_input_merge", False, None),
    ("b39", "decoder_input_merge", True, "e4m3"),
    ("b48", "up48", True, None),
    ("up56", "up56", True, None),
    ("up62", "up62", True, None),
    ("up66", "up66", True, None),
    ("b69", "w69", True, None),
    ("mrg70", "_rows", False, None),
    ("head", "head", False, "head4"),
]

print(f"{'tensor':<10} {'golden shape':<18} {'max|d|':>10} {'mean|d|':>10} {'rel':>9}  verdict")
worst = 0.0
for gpu_name, gold_name, f16, transform in MAP:
    g = load_gpu(gpu_name, f16)
    if g is None:
        print(f"{gpu_name:<10} MISSING")
        continue
    t = np.fromfile(ROOT / f"golden_{gold_name}.bin", dtype=np.float32)
    tshape = shapes[gold_name]
    t = t.reshape(tshape)
    if transform == "e4m3":
        t = e4m3(t)
    if transform == "head4":
        g = g.reshape(tshape[0], tshape[1], 16)[..., :4]
    t = t.reshape(-1)
    g = g.reshape(-1)
    if g.size != t.size:
        print(f"{gpu_name:<10} size mismatch: gpu {g.size} vs golden {t.size} ({tshape})")
        continue
    d = np.abs(g - t)
    mx, mn = d.max(), d.mean()
    rel = mx / (np.abs(t).max() + 1e-12)
    verdict = "OK" if rel < 2e-2 else ("meh" if rel < 1e-1 else "BAD")
    worst = max(worst, rel)
    print(f"{gpu_name:<10} {str(tshape):<18} {mx:>10.5f} {mn:>10.6f} {rel:>9.4f}  {verdict}")
print(f"\nworst rel: {worst:.4f}")
