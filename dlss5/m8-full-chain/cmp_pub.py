#!/usr/bin/env python3
import sys

import numpy as np

import golden as G

G.FRAG_SWIZZLE = np.arange(4096, dtype=np.intp)

st, outdir, blk = sys.argv[1], sys.argv[2] + "\\", int(sys.argv[3])
W = G.load_weights(st)
x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
inp = val
for i in range(0, blk):
    raw_prev = G._stem_block(inp, W, i, 32)
    inp = raw_prev if i == 0 else G.e4m3(raw_prev)
# golden raw for block blk (input inp)
rawg = G._stem_block(inp, W, blk, 32).reshape(-1)
pubg = G.e4m3(rawg).astype(np.float16)

gpu_raw = np.fromfile(outdir + "dbg_b1raw.bin", dtype=np.float32)
gpu_pub = np.fromfile(outdir + "dbg_b1pub.bin", dtype=np.float16)
print(f"blk{blk} raw fp32: maxabs {np.abs(rawg - gpu_raw).max():.4e} bitmatch {(rawg.view(np.uint32) == gpu_raw.view(np.uint32)).mean()*100:.4f}%")
same = pubg.view(np.uint16) == gpu_pub.view(np.uint16)
print(f"blk{blk} pub f16 : bitmatch {same.mean()*100:.4f}%")
bad = np.nonzero(~same)[0]
if bad.size:
    tok = bad // 32
    ch = bad % 32
    print("first bad tokens", tok[:20])
    print("bad token rows unique", np.unique(tok))
