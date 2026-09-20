#!/usr/bin/env python3
import sys
import numpy as np
import golden as G

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
raw0 = G._stem_block(val, W, 0, 32).reshape(-1)
gpu_raw0 = np.fromfile(outdir + "dbg_b0raw.bin", dtype=np.float32)
d = np.abs(raw0 - gpu_raw0)
print(f"raw0: bitmatch {(raw0.view(np.uint32) == gpu_raw0.view(np.uint32)).mean()*100:.4f}%  maxabs {d.max():.3e}")
