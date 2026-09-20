#!/usr/bin/env python3
import sys
import numpy as np
import golden as G

st = r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"
od = r"build\Release\out"
W = G.load_weights(st)
x = np.fromfile(od + "\\x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
r = G._stem_block(val, W, 0, 32).reshape(-1)
for name in ("raw_to0.bin", "raw_md17.bin", "dbg_b0raw.bin"):
    try:
        g = np.fromfile(od + "\\" + name, dtype=np.float32)
        print(f"{name}: maxabs vs golden {np.abs(r - g).max():.4e} bitmatch {(r.view(np.uint32) == g.view(np.uint32)).mean()*100:.4f}%")
    except Exception as e:
        print(name, "ERR", e)
