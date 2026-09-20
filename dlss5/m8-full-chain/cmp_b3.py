#!/usr/bin/env python3
"""cmp_b3.py — per-stage golden-vs-GPU compare for a stem window block.

GPU dumps come from a `--to N --dispdbg` run: dbg_b0{ffn,proj,sc,pr,mg,at,ab}.bin
then hold block N's window-attention internals (C=32, H=1 stem).

Usage: python cmp_b3.py <safetensors> <outdir> <blockidx>
"""
import sys

import numpy as np

import golden as G

WS = 8


def stage_compare(name, g, gpu_u8, f16):
    g = np.asarray(g, dtype=np.float32).reshape(-1)
    if f16:
        gpu = gpu_u8[: g.size * 2].view(np.uint16)
        gg2 = g.astype(np.float16).view(np.uint16)
        same = (gg2 == gpu[: gg2.size])
        bit = same.mean()
        gv = g.astype(np.float16).astype(np.float32)
        dv = gv - gpu[: gg2.size].astype(np.float32)
        print(f"  {name:<10} bitmatch {bit*100:8.4f}%  maxabs {np.abs(dv).max():.3e}")
        return bit >= 0.999
    gpu = gpu_u8[: g.size * 4].view(np.float32)
    same = (g.view(np.uint32) == gpu[: g.size].view(np.uint32))
    bit = same.mean()
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
    raw0 = G._stem_block(val, W, 0, 32)
    val = raw0
    for i in (1, 2):
        val = G.e4m3(G._stem_block(val, W, i, 32))
    # now val = input of block `blk` for blk in (1..4)
    for i in range(3, blk):
        val = G.e4m3(G._stem_block(val, W, i, 32)) if i != 4 else val

    # --- replicate _stem_block up to attention for block blk
    p = f"block{blk}.layer0"
    w1, w2 = W[p + ".weight1"], W[p + ".weight2"]
    fcos = W[p + ".ffn_cos_skip"]
    branch = G.e4m3(G.gate_activation(G.half_rounded(val) @ w1)).astype(np.float32) @ w2
    ffn = branch + val * fcos
    if blk >= 4:
        val = val  # b4 input not e4m3-published (b3 pub=true actually)
    # NOTE: b3 publishes e4m3(raw3) -> b4 input IS e4m3; handled above by loop for blk>3

    # --- attention stages
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

    def load(name, n):
        return np.fromfile(outdir + name, dtype=np.uint8)[:n]

    n_pr = Wn * 64 * 3 * C * 4
    n_sc = Wn * 64 * 64 * 4
    n_prw = Wn * 64 * 64 * 2
    n_mg = Wn * 64 * C * 4
    n_at = Wn * 64 * C * 2
    n_ab = Wn * 64 * C * 4

    g_ffn = ffn
    gpu_ffn = np.fromfile(outdir + "dbg_b0ffn.bin", dtype=np.uint8)
    ok = True
    print("stage-by-stage (golden vs GPU dbg):")
    ok &= stage_compare("ffn", g_ffn, gpu_ffn, False)
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

    # raw3: GPU --to 3 stops before b4; b3 publish wrote e4m3(raw3) to oG1.
    # golden raw for blk:
    g_raw = out
    # dbg_b0raw only holds b0; instead compare b3 published value via nothing.
    # So just report golden raw range and check ffn input consistency instead.
    print(f"golden raw{blk}: min {g_raw.min():.4f} max {g_raw.max():.4f}")
    print("ALL-STAGES-OK" if ok else "STAGE-DIVERGENCE")


if __name__ == "__main__":
    main()
