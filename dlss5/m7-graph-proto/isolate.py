#!/usr/bin/env python3
"""isolate.py — M7b run3: feed GPU-dumped proj through golden's cosine_publish,
compare bitwise vs GPU-dumped q16/k16/v16 in BOTH memory orders. Splits blame:
cosine.comp vs proj layout vs compare layout."""
import math
import sys

import numpy as np

sys.path.insert(0, r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto")
from golden import cosine_publish, e4m3, half_rounded, load_weights

TOK, CH, HEADS, HDIM = 288, 1024, 32, 32
OUT = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out"
ST = r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"

W = load_weights(ST)
attn_scale = W["block31.layer2.attn_scale"]
qscale = attn_scale * np.float32(math.sqrt(CH // HEADS))

proj = np.fromfile(OUT + r"\gpu_proj.bin", dtype=np.float32).reshape(TOK, 3072)
print("proj sanity: gpu_proj vs golden-consistent input -> compute both layouts")

q = proj[:, 0:CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)      # (H,T,32)
k = proj[:, CH:2 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
v = proj[:, 2 * CH:3 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)

q16 = cosine_publish(np.ascontiguousarray(q), qscale)   # (H,T,32) fp32 e4m3-valued
k16 = cosine_publish(np.ascontiguousarray(k))
v16 = e4m3(np.ascontiguousarray(v))

for name, g in [("q16", q16), ("k16", k16), ("v16", v16)]:
    gpu = np.fromfile(f"{OUT}\\gpu_{name}.f16", dtype=np.float16).reshape(TOK, CH)
    # (a) golden order: flatten (H,T,D) — what golden.py compare currently does
    a = g.reshape(-1).astype(np.float16)
    gu = gpu.reshape(-1)
    bm_a = (a.view(np.uint16) == gu.view(np.uint16)).mean()
    # (b) token-major order: transpose back to (T,H,D) -> (T,CH), GPU dump order
    b = np.ascontiguousarray(g.transpose(1, 0, 2)).reshape(-1).astype(np.float16)
    bm_b = (b.view(np.uint16) == gu.view(np.uint16)).mean()
    # value-set check ignoring order: sort both, compare
    sa = np.sort(a.view(np.uint16)); sb = np.sort(gu.view(np.uint16))
    bm_set = (sa == sb).mean()
    print(f"{name}: bitmatch (H,T,D)-order={bm_a*100:.4f}%  (T,H,D)-order={bm_b*100:.4f}%  "
          f"sorted-set={bm_set*100:.4f}%")
