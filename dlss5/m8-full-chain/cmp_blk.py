#!/usr/bin/env python3
"""cmp_blk.py — correct per-stage compare for stem block N.

GPU: run m8proto with --maxdisp set so the chain freezes just after block N's
dGather. dbg_b0{ffn,proj,sc,pr,mg,at,ab}.bin then hold block N's internals.
"""
import sys

import numpy as np

import golden as G

# GPU consumes the bias raw (validated b0 bit-exact); make the _stem_attn chain raw too.
G.FRAG_SWIZZLE = np.arange(4096, dtype=np.intp)


def stage_compare(name, g, gpu_u8, f16):
    g = np.asarray(g, dtype=np.float32).reshape(-1)
    if f16:
        gpu = gpu_u8[: g.size * 2].view(np.uint16)
        gg2 = g.astype(np.float16).view(np.uint16)
        bit = (gg2 == gpu[: gg2.size]).mean()
        dv = g.astype(np.float16).astype(np.float32) - gpu[: gg2.size].astype(np.float32)
        print(f"  {name:<10} bitmatch {bit*100:8.4f}%  maxabs {np.abs(dv).max():.3e}")
        return bit >= 0.999
    gpu = gpu_u8[: g.size * 4].view(np.float32)
    bit = (g.view(np.uint32) == gpu[: g.size].view(np.uint32)).mean()
    d = np.abs(g - gpu[: g.size])
    print(f"  {name:<10} bitmatch {bit*100:8.4f}%  maxabs {d.max():.3e}")
    return bit >= 0.999


def main():
    st, outdir, blk = sys.argv[1], sys.argv[2] + "\\", int(sys.argv[3])
    W = G.load_weights(st)
    C = 32
    heads = 1
    oy, ox = G.stem_origin(blk)
    print(f"block {blk} origin=({oy},{ox})")

    x = np.fromfile(outdir + "x.bin", dtype=np.float32).reshape(-1, 16)
    val = G.half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
    # rebuild inputs: in[0]=adapter val; in[i]=e4m3(raw[i-1])
    raw_prev = None
    inp = val
    for i in range(0, blk):
        raw_prev = G._stem_block(inp, W, i, 32)
        inp = raw_prev if i == 0 else G.e4m3(raw_prev)
    # now inp = input of block blk
    if blk == 0:
        inp = val

    p = f"block{blk}.layer0"
    w1, w2 = W[p + ".weight1"], W[p + ".weight2"]
    fcos = W[p + ".ffn_cos_skip"]
    branch = G.e4m3(G.gate_activation(G.half_rounded(inp) @ w1)).astype(np.float32) @ w2
    ffn = branch + inp * fcos

    qkv = W[p + ".qkv_weight"]
    scale = W[p + ".attn_scale"]
    bias = W[p + ".attn_bias"]
    proj = W[p + ".projection_weight"]
    acos = W[p + ".attn_cos_skip"]
    xw = G.half_rounded(ffn)
    wins, pt, pl = G._partition(xw, oy, ox, C)
    pr = wins @ qkv
    q = pr[..., :C].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
    k = pr[..., C:2 * C].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
    v = pr[..., 2 * C:].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
    qn = G.cosine_normalize(q)
    q16 = G.e4m3(G.half_rounded(qn * G.half_rounded(scale).reshape(1, heads, 1, 1)))
    k16 = G.cosine_publish(np.ascontiguousarray(k))
    v16 = G.e4m3(np.ascontiguousarray(v))
    scores = q16 @ k16.swapaxes(-1, -2) + bias[None]
    probs = G.softmax(scores).astype(np.float32)
    merged = (probs @ v16).transpose(0, 2, 1, 3).reshape(-1, 64, C)
    att16 = G.e4m3(np.ascontiguousarray(merged)).astype(np.float32)
    br = att16 @ proj
    out = G._reverse(br, pt, pl, C) + ffn * acos

    Wn = wins.shape[0]
    print(f"windows={Wn} padTop={pt} padLeft={pl}")

    ok = True
    print("stage-by-stage (golden vs GPU dbg):")
    ok &= stage_compare("ffn", ffn, np.fromfile(outdir + "dbg_b0ffn.bin", dtype=np.uint8), False)
    ok &= stage_compare("qkv", pr.reshape(Wn * 64, 3 * C),
                        np.fromfile(outdir + "dbg_b0proj.bin", dtype=np.uint8), False)
    ok &= stage_compare("scores", scores.reshape(-1),
                        np.fromfile(outdir + "dbg_b0sc.bin", dtype=np.uint8), False)
    ok &= stage_compare("probs", probs.reshape(-1),
                        np.fromfile(outdir + "dbg_b0pr.bin", dtype=np.uint8), True)
    ok &= stage_compare("merged", merged.reshape(-1),
                        np.fromfile(outdir + "dbg_b0mg.bin", dtype=np.uint8), False)
    ok &= stage_compare("att16", att16.reshape(-1),
                        np.fromfile(outdir + "dbg_b0at.bin", dtype=np.uint8), True)
    ok &= stage_compare("branch", br.reshape(-1),
                        np.fromfile(outdir + "dbg_b0ab.bin", dtype=np.uint8), False)
    if blk == 0:
        ok &= stage_compare("raw0", out.reshape(-1),
                            np.fromfile(outdir + "dbg_b0raw.bin", dtype=np.uint8), False)
    print("ALL-STAGES-OK" if ok else "STAGE-DIVERGENCE")


if __name__ == "__main__":
    main()
