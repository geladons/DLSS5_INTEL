#!/usr/bin/env python3
"""sensitivity.py - how much does the global family amplify GEMM ordering noise?
Golden-fp32 vs golden-with-float64-accumulated GEMMs: per-block pub bitmatch."""
import math
import sys
import numpy as np

sys.path.insert(0, ".")
import golden as G
from golden import (TOK, CH, HEADS, HDIM, half_rounded, e4m3, gate_activation,
                    cosine_publish, softmax, load_weights, _origin, _plain_block,
                    _branched_block, _split_block)


def matmul(a, b):
    return (a.astype(np.float64) @ b.astype(np.float64)).astype(np.float32)


def stage64(W, x, idx):
    p = f"block{idx}"
    qscale = W[p + ".layer2.attn_scale"] * np.float32(math.sqrt(CH // HEADS))
    x_hr = half_rounded(x)
    acc = matmul(x_hr, W[p + ".layer0.weight"])
    h = e4m3(gate_activation(acc))
    branch = matmul(h, W[p + ".layer1.weight"])
    ffn = branch + x * W[p + ".layer1.ffn_cos_skip"]
    proj = matmul(half_rounded(ffn), W[p + ".layer2.qkv_weight"])
    q = proj[:, 0:CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    k = proj[:, CH:2 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    v = proj[:, 2 * CH:3 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    q16 = cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)
    k16 = cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
    v16 = e4m3(np.ascontiguousarray(v)).astype(np.float16)
    scores = np.clip(matmul(q16.astype(np.float32), k16.astype(np.float32).swapaxes(-1, -2)), -3.0, 3.0)
    probs = softmax(scores).astype(np.float16)
    ctx = matmul(probs.astype(np.float32), v16.astype(np.float32))
    merged = ctx.transpose(1, 0, 2).reshape(TOK, CH)
    attended16 = e4m3(merged).astype(np.float16)
    attn_branch = matmul(attended16.astype(np.float32), W[p + ".layer4.projection_weight"])
    out = attn_branch + ffn * W[p + ".layer4.attn_cos_skip"]
    return e4m3(out).astype(np.float16)


def main():
    W = load_weights(sys.argv[1] if len(sys.argv) > 1 else
                     r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
    # shared input: golden chain up to b30
    import cmp_b31
    xin = cmp_b31.run_to_b30(W)

    def run(fn):
        cur = xin.astype(np.float32)
        pubs = []
        for idx in range(31, 39):
            if fn == 32:
                s = cmp_b31.stage(W, cur, idx)["pub"]
            else:
                s = stage64(W, cur, idx)
            pubs.append(s)
            cur = s.astype(np.float32)
        return pubs

    p32 = run(32)
    p64 = run(64)
    print("golden-fp32 vs golden-fp64GEMM per-block pub:")
    for i, (a, b) in enumerate(zip(p32, p64)):
        eq, w1s, mx = cmp_b31.bitmatch(a, b)
        print(f"b{31+i}   bitmatch {eq:7.3f}%  within1 {w1s:7.3f}%  maxabs {mx:.4g}")


if __name__ == "__main__":
    main()
