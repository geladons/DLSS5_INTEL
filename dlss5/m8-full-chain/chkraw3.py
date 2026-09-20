#!/usr/bin/env python3
import hashlib
import sys

import numpy as np

import golden as G

st = r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"
od = r"build\Release\out"
xb = open(od + "\\x.bin", "rb").read()
print("x.bin sha1", hashlib.sha1(xb).hexdigest())
W = G.load_weights(st)
x = np.frombuffer(xb, dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
r = G._stem_block(val, W, 0, 32).reshape(-1)
print("golden raw0 sha1", hashlib.sha1(r.tobytes()).hexdigest(), "range", r.min(), r.max())
g = np.fromfile(od + "\\dbg_b0raw.bin", dtype=np.float32)
print("gpu   raw0 sha1", hashlib.sha1(g.tobytes()).hexdigest(), "range", g.min(), g.max())
print("maxabs", np.abs(r - g).max())
