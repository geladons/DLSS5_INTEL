#!/usr/bin/env python3
import sys
import numpy as np
import golden as G

st, outdir = sys.argv[1], sys.argv[2] + "\\"
W = G.load_weights(st)
x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
raw0 = G._stem_block(val, W, 0, 32)
gpu_raw0 = np.fromfile(outdir + "dbg_b0raw.bin", dtype=np.float32)
d = np.abs(raw0.reshape(-1) - gpu_raw0)
same = raw0.reshape(-1).view(np.uint32) == gpu_raw0.view(np.uint32)
print(f"raw0: bitmatch {same.mean()*100:.4f}%  maxabs {d.max():.3e}")
print(f"gpu_raw0 range [{gpu_raw0.min():.3f},{gpu_raw0.max():.3f}]  golden [{raw0.min():.3f},{raw0.max():.3f}]")

# also b1 ffn input on device: b1 takes raw0 fp32 (oB0RAW) directly.
# golden b1 ffn:
p = "block1.layer0"
branch = G.e4m3(G.gate_activation(G.half_rounded(raw0) @ W[p + ".weight1"])).astype(np.float32) @ W[p + ".weight2"]
ffn1 = branch + raw0 * W[p + ".ffn_cos_skip"]
gpu_ffn1 = np.fromfile(outdir + "dbg_b0ffn.bin", dtype=np.float32)
d2 = np.abs(ffn1.reshape(-1) - gpu_ffn1)
print(f"b1ffn: maxabs {d2.max():.3e}  gpu range [{gpu_ffn1.min():.2f},{gpu_ffn1.max():.2f}] golden [{ffn1.min():.2f},{ffn1.max():.2f}]")

# weight spot check: what the device SHOULD have. Compare against what pack
# layout would place. Just print a few weight values for manual inspection.
w1 = W[p + ".weight1"]
print("block1 w1 shape", w1.shape, "first4", w1.reshape(-1)[:4])
w0 = W["block0.layer0.weight1"]
print("block0 w1 shape", w0.shape, "first4", w0.reshape(-1)[:4])
