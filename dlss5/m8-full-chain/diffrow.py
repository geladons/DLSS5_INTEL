#!/usr/bin/env python3
"""diff two dbg_b0raw.bin dumps to see which region changed."""
import sys

import numpy as np

a = np.fromfile(sys.argv[1], dtype=np.float32)
b = np.fromfile(sys.argv[2], dtype=np.float32)
diff = np.abs(a - b) > 1e-6
idx = np.nonzero(diff)[0]
print(f"total diff {diff.sum()} / {a.size}")
if idx.size:
    print(f"first {idx[0]} last {idx[-1]}")
    # histogram by 576-token (32ch) row groups
    rows = idx // 32
    print("rows changed:", np.unique(rows)[:50])
