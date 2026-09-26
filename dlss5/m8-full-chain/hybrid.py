#!/usr/bin/env python3
"""hybrid.py - golden decoder fed with the GPU's ACTUAL b38 dump.
If decoder/head then match the GPU dumps ~100%, all their divergence is
inherited from the global family's chaotic amplification, not decoder math."""
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys
import numpy as np

sys.path.insert(0, ".")
from golden import (TOK as TOK_CHAIN, half_rounded, e4m3, load_weights, _origin,
                    _plain_block, _branched_block, _split_block, _up)
import cmp_b31

OUT = "build/Release/out/"


def main():
    W = load_weights(sys.argv[1] if len(sys.argv) > 1 else
                     r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
    # bit-exact upstream (stem/enc/bottleneck verified 100%): recompute skips
    b30, xin = cmp_b31.run_to_b30(W)   # b30 (288,512) split_skip; xin = bridge out (288,1024)
    # rebuild skips + frs exactly as run_full_chain does
    x = np.fromfile(OUT + "x.bin", dtype=np.float32).reshape(TOK_CHAIN, 16)
    val = half_rounded(x) @ W["block0.layer0.input_adapter_weight"]
    raw0 = _plain_block(val, W, 0, 1, 0, 0, False)
    frs = e4m3(raw0)
    cur = raw0
    for i in (1, 2, 3):
        cur = _plain_block(cur, W, i, 1, *_origin(i), True)
    skip0 = cur
    raw4 = _plain_block(cur, W, 4, 1, *_origin(4), False)
    cur = e4m3(e4m3(raw4) @ W["block4.layer0.weight0"]).astype(np.float16)
    for i in (5, 6, 7):
        cur = _branched_block(cur, W, i, 2, *_origin(i), True)
    skip1 = cur
    raw8 = _branched_block(cur, W, 8, 2, *_origin(8), False)
    cur = e4m3(e4m3(raw8) @ W["block8.layer0.weight0"]).astype(np.float16)
    for i in range(9, 14):
        cur = _branched_block(cur, W, i, 4, *_origin(i), True)
    skip2 = cur
    raw14 = _branched_block(cur, W, 14, 4, *_origin(14), False)
    cur = e4m3(e4m3(raw14) @ W["block14.layer0.weight0"]).astype(np.float16)
    for i in range(15, 22):
        cur = _branched_block(cur, W, i, 8, *_origin(i), True)
    skip3 = cur
    b30 = b30.astype(np.float16)   # bottleneck boundary (bit-exact vs GPU)

    # ---- decoder fed with GPU b38 ----
    b38 = np.fromfile(OUT + "gpu_b38.f16", dtype=np.float16).reshape(TOK_CHAIN, 1024)
    cur = e4m3(half_rounded(b38.astype(np.float32)) @ W["block39.layer0.conv_weight"]
               + b30.astype(np.float32) * W["block39.layer0.inp_upsample_sin"])
    b39 = cur.astype(np.float16)
    for i in range(40, 48):
        cur = _split_block(cur, W, i, *_origin(i), True)
    cur = _up(cur, skip3, W, 48)
    cur = _branched_block(cur, W, 48, 8, 0, 0, True)
    b48 = cur.astype(np.float16)
    for i in range(49, 56):
        cur = _branched_block(cur, W, i, 8, *_origin(i), True)
    cur = _up(cur, skip2, W, 56)
    cur = _branched_block(cur, W, 56, 4, 0, -4, True)
    for i in range(57, 62):
        cur = _branched_block(cur, W, i, 4, *_origin(i), True)
    cur = _up(cur, skip1, W, 62)
    cur = _branched_block(cur, W, 62, 2, 0, 0, True)
    for i in range(63, 66):
        cur = _branched_block(cur, W, i, 2, *_origin(i), True)
    cur = _up(cur, skip0, W, 66)
    cur = _plain_block(cur, W, 66, 1, 0, 0, True)
    for i in range(67, 70):
        cur = _plain_block(cur, W, i, 1, *_origin(i), True)
    b69 = cur.astype(np.float16)
    merged70 = b69.astype(np.float32) * W["block70.layer0.inp_merge_sin"] \
        + frs * W["block70.layer0.inp_merge_cos"]
    raw70 = _plain_block(merged70, W, 70, 1, -4, -4, False)
    t70 = half_rounded(raw70)
    head = t70[:, :16] @ W["block70.layer0.out_gain"] \
        + t70[:, 16:] @ W["block70.layer0.out_conv_weight"]
    head = np.concatenate([head, np.zeros((TOK_CHAIN, 12), np.float32)], axis=1)

    for name, g, gold, shp in [("b39", b39, None, (TOK_CHAIN, 512)),
                               ("b48", b48, None, (TOK_CHAIN, 256)),
                               ("b69", b69, None, (TOK_CHAIN, 32))]:
        gpu = np.fromfile(f"{OUT}gpu_{name}.f16", dtype=np.float16).reshape(shp)
        eq, w1s, mx = cmp_b31.bitmatch(gpu, g)
        print(f"{name}: hybrid-golden vs GPU  bitmatch {eq:7.3f}%  within1 {w1s:7.3f}%  maxabs {mx:.4g}")
    for name, g in [("merged70", merged70), ("head", head)]:
        gpu = np.fromfile(f"{OUT}gpu_{name}.bin", dtype=np.float32).reshape(g.shape)
        d = np.abs(gpu - g)
        print(f"{name}: max-abs {d.max():.6g} mean-rel {(d/np.maximum(np.abs(g),1e-6)).mean():.6g}")


if __name__ == "__main__":
    main()
