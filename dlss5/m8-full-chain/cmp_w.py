#!/usr/bin/env python3
import sys

import numpy as np

import golden as G

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
for name, devf, wf in (
    ("block1.w1", "dbg_w1b1.bin", "block1.layer0.weight1"),
    ("block2.w1", "dbg_w1b2.bin", "block2.layer0.weight1"),
    ("block2.qkv", "dbg_qkvb2.bin", "block2.layer0.qkv_weight"),
    ("block2.bias", "dbg_biasb2.bin", "block2.layer0.attn_bias"),
):
    g = W[wf].reshape(-1)
    d = np.fromfile(outdir + devf, dtype=np.float16).reshape(-1)
    n = min(g.size, d.size)
    same = (g[:n].astype(np.float16).view(np.uint16) == d[:n].view(np.uint16))
    md = np.abs(g[:n].astype(np.float32) - d[:n].astype(np.float32))
    print(f"{name}: bitmatch {same.mean()*100:.4f}%  maxabs {md.max():.3e}")
