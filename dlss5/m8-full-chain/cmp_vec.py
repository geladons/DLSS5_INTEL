#!/usr/bin/env python3
import sys

import numpy as np

import golden as G

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
for name, devf, wf in (
    ("block1.ffn_cos", "dbg_fc1.bin", "block1.layer0.ffn_cos_skip"),
    ("block2.ffn_cos", "dbg_fc2.bin", "block2.layer0.ffn_cos_skip"),
    ("block2.attn_cos", "dbg_ac2.bin", "block2.layer0.attn_cos_skip"),
):
    g = W[wf].reshape(-1)
    d = np.fromfile(outdir + devf, dtype=np.float32).reshape(-1)
    n = min(g.size, d.size)
    print(f"{name}: bitmatch {(g[:n].view(np.uint32) == d[:n].view(np.uint32)).mean()*100:.4f}% maxabs {np.abs(g[:n]-d[:n]).max():.3e} g[:4]={g[:4]} d[:4]={d[:4]}")
