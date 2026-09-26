#!/usr/bin/env python3
"""quant2.py — verify >1-e4m3-step flips are confined to the subnormal region."""
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import math
import sys

import numpy as np

sys.path.insert(0, r"" + str(_REPO) + r"\dlss5\m7-graph-proto")
from golden import (cosine_publish, e4m3, gate_activation, half_rounded,
                    load_weights, softmax, TOK, CH, HEADS, HDIM)

OUT = r"" + str(_REPO) + r"\dlss5\m7-graph-proto\build\Release\out"
ST = r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors"

W = load_weights(ST)
x = np.fromfile(OUT + r"\x.bin", dtype=np.float32).reshape(TOK, CH)
w0 = W["block31.layer0.weight"]; w1 = W["block31.layer1.weight"]
qkv = W["block31.layer2.qkv_weight"]; proj_w = W["block31.layer4.projection_weight"]
attn_cos = W["block31.layer4.attn_cos_skip"]; ffn_cos = W["block31.layer1.ffn_cos_skip"]
x_hr = half_rounded(x)
h = e4m3(gate_activation(x_hr @ w0)).astype(np.float16)
branch = h.astype(np.float32) @ w1
ffn_out = branch + x * ffn_cos
ffn_hr = half_rounded(ffn_out)
proj = ffn_hr @ qkv
q = proj[:, 0:CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
k = proj[:, CH:2 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
v = proj[:, 2 * CH:3 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
qs = W["block31.layer2.attn_scale"] * np.float32(math.sqrt(32))
q16 = cosine_publish(np.ascontiguousarray(q), qs).astype(np.float16)
k16 = cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
v16 = e4m3(np.ascontiguousarray(v)).astype(np.float16)
scores = np.matmul(q16.astype(np.float32), k16.astype(np.float32).swapaxes(-1, -2))
probs = softmax(np.clip(scores, -3, 3)).astype(np.float16)
ctx = np.matmul(probs.astype(np.float32), v16.astype(np.float32))
merged = ctx.transpose(1, 0, 2).reshape(TOK, CH)
attended16 = e4m3(merged).astype(np.float16)
attn_branch = attended16.astype(np.float32) @ proj_w
block_raw = attn_branch + ffn_out * attn_cos
block16 = e4m3(block_raw).astype(np.float16)

for name, g in [("attended16", attended16), ("block16", block16)]:
    gu = np.fromfile(f"{OUT}\\gpu_{name}.f16", dtype=np.float16).reshape(-1)
    gg = g.reshape(-1)
    d = np.abs(gg.astype(np.float32) - gu.astype(np.float32))
    step = np.maximum(np.abs(gg.astype(np.float32)), np.float32(2.0 ** -9)) / 8.0
    st = d / step
    m = st > 1.0
    print(f"{name}: >1step={int(m.sum())}  max|golden|={np.abs(gg[m]).max() if m.any() else 0:.6f}"
          f"  all-in-subnormal(<2^-6)={bool((np.abs(gg[m]) < 2**-6).all()) if m.any() else True}"
          f"  max-abs-d={d[m].max() if m.any() else 0:.6f}")

gs = np.fromfile(OUT + r"\gpu_scores.bin", dtype=np.float32).reshape(HEADS, TOK, TOK)
gc = np.clip(gs, -3, 3)
s2 = np.clip(scores, -3, 3)
d = np.abs(gc - s2)
print(f"scores(clipped): elems d>0.02: {int((d > 0.02).sum())}  max-d {d.max():.4f}  "
      f"mean-rel {(d / np.maximum(np.abs(s2), 1e-6)).mean():.6f}")
