#!/usr/bin/env python3
import sys

import numpy as np

import golden as G

G.FRAG_SWIZZLE = np.arange(4096, dtype=np.intp)

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
raw0 = G._stem_block(val, W, 0, 32)
raw1 = G._stem_block(raw0, W, 1, 32)
inp2 = G.e4m3(raw1)
p = "block2.layer0"
w1, w2 = W[p + ".weight1"], W[p + ".weight2"]
fcos = W[p + ".ffn_cos_skip"]
g_g3 = G.e4m3(G.gate_activation(G.half_rounded(inp2) @ w1)).astype(np.float16).reshape(-1)
g_g4mid = G.e4m3(G.gate_activation(G.half_rounded(inp2) @ w1)).astype(np.float32) @ w2
g_ffn = g_g4mid + inp2 * fcos

d_g3 = np.fromfile(outdir + "dbg_b2g3.bin", dtype=np.float16).reshape(-1)
d_g4 = np.fromfile(outdir + "dbg_b2g4mid.bin", dtype=np.float32).reshape(-1)
d_ffn = np.fromfile(outdir + "dbg_b0ffn.bin", dtype=np.float32).reshape(-1)

print(f"b2 gate(oG3)  : bitmatch {(g_g3.view(np.uint16) == d_g3.view(np.uint16)).mean()*100:.4f}%")
print(f"b2 branch(oG4): bitmatch {(g_g4mid.reshape(-1).view(np.uint32) == d_g4.view(np.uint32)).mean()*100:.4f}% maxabs {np.abs(g_g4mid.reshape(-1) - d_g4).max():.3e}")
print(f"b2 ffn(oG4)   : maxabs {np.abs(g_ffn.reshape(-1) - d_ffn).max():.3e}")
