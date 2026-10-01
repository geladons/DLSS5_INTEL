import os
import numpy as np

out_dir = "dlss5/m11d/build/Release/out"
files = [
    ("live_featV.bin", np.float32),
    ("live_x16.bin", np.float16),
    ("live_ada.bin", np.float32),
    ("live_g2.bin", np.float32),
    ("live_frs.f16", np.float16),
    ("live_b4ds.f16", np.float16),
    ("live_b22ds.f16", np.float16),
    ("live_b38.f16", np.float16),
    ("live_b48.f16", np.float16),
    ("live_b69.f16", np.float16),
    ("live_mrg70.bin", np.float32),
    ("live_head.bin", np.float32),
]

print("=== Checking where NaN first appears in the chain ===")
for fname, dt in files:
    p = os.path.join(out_dir, fname)
    if not os.path.exists(p):
        print(f"{fname:<16}: missing")
        continue
    data = np.fromfile(p, dtype=dt)
    nans = int(np.isnan(data).sum())
    infs = int(np.isinf(data).sum())
    min_v = float(np.nanmin(data)) if nans < data.size else float("nan")
    max_v = float(np.nanmax(data)) if nans < data.size else float("nan")
    status = f"NANs: {nans}/{data.size} ({100.0*nans/data.size:.1f}%)" if nans > 0 else "OK"
    print(f"{fname:<16}: {status:<24} min={min_v:+.4f}, max={max_v:+.4f}")
