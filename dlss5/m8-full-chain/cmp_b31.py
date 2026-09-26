#!/usr/bin/env python3
"""cmp_b31.py - stage-by-stage golden vs GPU dbg31_* dump compare for block 31."""
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import math
import sys
import numpy as np

sys.path.insert(0, ".")
from golden import (TOK, CH, HEADS, HDIM, half_rounded, e4m3, gate_activation,
                    cosine_publish, softmax, load_weights, _origin, _plain_block,
                    _branched_block, _split_block)

OUT = "build/Release/out/"


def gpu(name, dt, shape):
    return np.fromfile(OUT + name, dtype=dt).reshape(shape)


def f16(x):
    return np.asarray(x, np.float32).astype(np.float16)


def bitmatch(a, b):
    a = f16(a).reshape(-1)
    b = f16(b).reshape(-1)
    eq = np.sum(a.view(np.uint16) == b.view(np.uint16)) / a.size
    af, bf = a.astype(np.float32), b.astype(np.float32)
    step = np.abs(af - bf)
    denom = np.maximum(np.abs(af), 1e-6)
    within1 = np.mean(step <= denom * 0.125 + 2 ** -9)
    return eq * 100, within1 * 100, float(np.max(np.abs(af - bf)))


def stage(W, x, idx):
    """Golden block-31 internals; x = f16 carrier (288,1024) as f32."""
    p = f"block{idx}"
    w0 = W[p + ".layer0.weight"]
    w1 = W[p + ".layer1.weight"]
    ffn_cos = W[p + ".layer1.ffn_cos_skip"]
    qkv = W[p + ".layer2.qkv_weight"]
    attn_scale = W[p + ".layer2.attn_scale"]
    proj_w = W[p + ".layer4.projection_weight"]
    attn_cos = W[p + ".layer4.attn_cos_skip"]
    qscale = attn_scale * np.float32(math.sqrt(CH // HEADS))

    x_hr = half_rounded(x)
    acc = x_hr @ w0
    h = e4m3(gate_activation(acc))
    branch = h @ w1
    ffn = branch + x * ffn_cos                       # x fp32 = widened f16 carrier
    ffn_hr = half_rounded(ffn)
    proj = ffn_hr @ qkv
    q = proj[:, 0:CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    k = proj[:, CH:2 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    v = proj[:, 2 * CH:3 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
    q16 = cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)
    k16 = cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
    v16 = e4m3(np.ascontiguousarray(v)).astype(np.float16)
    scores = np.clip(q16.astype(np.float32) @ k16.astype(np.float32).swapaxes(-1, -2), -3.0, 3.0)
    probs = softmax(scores).astype(np.float16)
    ctx = probs.astype(np.float32) @ v16.astype(np.float32)
    merged = ctx.transpose(1, 0, 2).reshape(TOK, CH)
    attended16 = e4m3(merged).astype(np.float16)
    attn_branch = attended16.astype(np.float32) @ proj_w
    out = attn_branch + ffn * attn_cos
    pub = e4m3(out).astype(np.float16)
    # token-major views for GPU compare
    q16_t = q16.transpose(1, 0, 2).reshape(TOK, CH)
    k16_t = k16.transpose(1, 0, 2).reshape(TOK, CH)
    v16_t = v16.transpose(1, 0, 2).reshape(TOK, CH)
    probs_h = probs.transpose(1, 0, 2).reshape(HEADS * TOK, TOK)  # GPU [h][tq][tk]
    return dict(hg=h, ffn=ffn, q16=q16_t, k16=k16_t, v16=v16_t,
                probs=probs_h, attended=attended16, pub=pub)


def run_to_b30(W):
    x = np.fromfile(OUT + "x.bin", dtype=np.float32).reshape(TOK, 16)
    val = half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
    cur = _plain_block(val, W, 0, 1, 0, 0, False)
    for i in (1, 2, 3):
        cur = _plain_block(cur, W, i, 1, *_origin(i), True)
    raw4 = _plain_block(cur, W, 4, 1, *_origin(4), False)
    cur = e4m3(e4m3(raw4) @ W["block4.layer0.weight0"]).astype(np.float16)
    for i in (5, 6, 7):
        cur = _branched_block(cur, W, i, 2, *_origin(i), True)
    raw8 = _branched_block(cur, W, 8, 2, *_origin(8), False)
    cur = e4m3(e4m3(raw8) @ W["block8.layer0.weight0"]).astype(np.float16)
    for i in range(9, 14):
        cur = _branched_block(cur, W, i, 4, *_origin(i), True)
    raw14 = _branched_block(cur, W, 14, 4, *_origin(14), False)
    cur = e4m3(e4m3(raw14) @ W["block14.layer0.weight0"]).astype(np.float16)
    for i in range(15, 22):
        cur = _branched_block(cur, W, i, 8, *_origin(i), True)
    raw22 = _branched_block(cur, W, 22, 8, *_origin(22), False)
    cur = e4m3(e4m3(raw22) @ W["block22.layer0.weight0"]).astype(np.float16)
    for i in range(23, 31):
        cur = _split_block(cur, W, i, *_origin(i), True)
    b30 = cur.astype(np.float16)
    bridge = e4m3(b30.astype(np.float32) @ W["block30.layer4.weight"]).astype(np.float16)
    return b30, bridge


def main():
    W = load_weights(sys.argv[1] if len(sys.argv) > 1 else
                     r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
    b30, xin = cmp_b31.run_to_b30(W)
    xin = xin.astype(np.float32)
    print("in     ", end="")
    g_in = gpu("dbg31_in.bin", np.float16, (TOK, CH))
    eq, w1s, mx = bitmatch(xin, g_in)
    print(f"bitmatch {eq:7.3f}%  within1 {w1s:7.3f}%  maxabs {mx:.4g}")
    cur = xin
    for idx in range(31, 39):
        s = stage(W, cur, idx)
        g = gpu(f"dbg{idx}_pub.bin", np.float16, (TOK, CH))
        eq, w1s, mx = bitmatch(s["pub"], g)
        print(f"b{idx} pub bitmatch {eq:7.3f}%  within1 {w1s:7.3f}%  maxabs {mx:.4g}")
        cur = s["pub"].astype(np.float32)
        if idx == 31:
            g_hg = gpu("dbg31_hg.bin", np.float16, (TOK, 4096))
            for name, gg, gold in [("hg", g_hg, s["hg"]),
                                   ("ffn", gpu("dbg31_ffn.bin", np.float32, (TOK, CH)), s["ffn"]),
                                   ("q16", gpu("dbg31_q.bin", np.float16, (TOK, CH)), s["q16"]),
                                   ("k16", gpu("dbg31_k.bin", np.float16, (TOK, CH)), s["k16"]),
                                   ("v16", gpu("dbg31_v.bin", np.float16, (TOK, CH)), s["v16"]),
                                   ("attended", gpu("dbg31_at.bin", np.float16, (TOK, CH)), s["attended"])]:
                e2, w2, m2 = bitmatch(gold, gg)
                print(f"  {name:9s} bitmatch {e2:7.3f}%  within1 {w2:7.3f}%  maxabs {m2:.4g}")


if __name__ == "__main__":
    main()
