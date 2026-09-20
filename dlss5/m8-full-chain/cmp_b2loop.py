#!/usr/bin/env python3
import sys

import numpy as np

import golden as G

G.FRAG_SWIZZLE = np.arange(4096, dtype=np.intp)

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
fc = W["block2.layer0.ffn_cos_skip"].reshape(-1).astype(np.float32)
x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
raw0 = G._stem_block(val, W, 0, 32)
raw1 = G._stem_block(raw0, W, 1, 32)
inp2 = G.e4m3(raw1)
branchg = (G.e4m3(G.gate_activation(G.half_rounded(inp2) @ W["block2.layer0.weight1"])).astype(np.float32) @ W["block2.layer0.weight2"]).reshape(-1)

ffn = np.fromfile(outdir + "dbg_b0ffn.bin", dtype=np.float32).reshape(-1)   # oG4 @ maxdisp35 = final ffn
x16 = np.fromfile(outdir + "dbg_b2x16.bin", dtype=np.float16).reshape(-1)   # oG1 @ maxdisp35 = b2 input

print("dumped x16 vs golden inp2 bitmatch:",
      (x16.view(np.uint16) == inp2.reshape(-1).astype(np.float16).view(np.uint16)).mean())
expect = branchg + x16.astype(np.float32) * np.tile(fc, 288)
print("golden-branch + dumped-x16 vs dumped ffn: maxabs", np.abs(expect - ffn).max())
gffn = branchg + inp2.reshape(-1) * fc
print("golden ffn vs dumped ffn: maxabs", np.abs(gffn - ffn).max())
# where do dumped ffn and (branch+x16*fc) disagree?
d = np.abs(expect - ffn).reshape(288, 32)
bad = np.nonzero(d.max(axis=1) > 1e-3)[0]
print("bad tokens:", bad[:40])
print("as (y,x):", [(int(t) // 12, int(t) % 12) for t in bad[:24]])
