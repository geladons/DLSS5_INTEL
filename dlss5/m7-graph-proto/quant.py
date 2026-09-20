#!/usr/bin/env python3
"""quant.py — M7b run3: honest outlier forensics after the layout fix."""
import sys
import numpy as np

sys.path.insert(0, r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto")
from golden import (cosine_publish, e4m3, half_rounded, load_weights, softmax,
                    TOK, CH, HEADS, HDIM)
import math

OUT = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out"
ST = r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"

W = load_weights(ST)
attn_scale = W["block31.layer2.attn_scale"]
qscale = attn_scale * np.float32(math.sqrt(CH // HEADS))
x = np.fromfile(OUT + r"\x.bin", dtype=np.float32).reshape(TOK, CH)
w0 = W["block31.layer0.weight"]; w1 = W["block31.layer1.weight"]
qkv = W["block31.layer2.qkv_weight"]; proj_w = W["block31.layer4.projection_weight"]
attn_cos = W["block31.layer4.attn_cos_skip"]; ffn_cos = W["block31.layer1.ffn_cos_skip"]

x_hr = half_rounded(x)
h = e4m3(__import__("golden").gate_activation(x_hr @ w0)).astype(np.float16)
branch = h.astype(np.float32) @ w1
ffn_out = branch + x * ffn_cos
ffn_hr = half_rounded(ffn_out)
proj = ffn_hr @ qkv
q = proj[:, 0:CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
k = proj[:, CH:2 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
v = proj[:, 2 * CH:3 * CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
q16 = cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)
k16 = cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
v16 = e4m3(np.ascontiguousarray(v)).astype(np.float16)
scores = np.matmul(q16.astype(np.float32), k16.astype(np.float32).swapaxes(-1, -2))
raw_clip = np.clip(scores, -3.0, 3.0)
probs = softmax(np.clip(scores, -3.0, 3.0)).astype(np.float16)
ctx = np.matmul(probs.astype(np.float32), v16.astype(np.float32))
merged = ctx.transpose(1, 0, 2).reshape(TOK, CH)
attended16 = e4m3(merged).astype(np.float16)
attn_branch = attended16.astype(np.float32) @ proj_w
block_raw = attn_branch + ffn_out * attn_cos
block16 = e4m3(block_raw).astype(np.float16)

def steps_metric(gg, gu):
    d = np.abs(gg.astype(np.float32) - gu.astype(np.float32))
    step = np.maximum(np.abs(gg.astype(np.float32)), np.float32(2.0 ** -9)) / 8.0
    return d / step

print("== contract tensors ==")
for name, g in [("q16", q16), ("k16", k16), ("v16", v16), ("probs", probs),
                ("attended16", attended16), ("block16", block16)]:
    if name in ("q16", "k16", "v16"):   # (H,T,D) -> token-major; probs is (H,T,T)
        g = np.ascontiguousarray(g).transpose(1, 0, 2)
    gpu = np.fromfile(f"{OUT}\\gpu_{name}.f16", dtype=np.uint16).reshape(g.shape)
    gg = g.reshape(-1)
    gu16 = gpu.reshape(-1).view(np.float16)
    same = (gg.view(np.uint16) == gpu.reshape(-1))
    ndiff = int((~same).sum())
    st = steps_metric(gg, gu16)
    within1 = (st <= 1.0).mean()
    print(f"{name}: bitmatch={same.mean()*100:.4f}%  ndiff={ndiff}  <=1step={within1*100:.4f}%"
          f"  maxstep={st.max():.1f}")
    if ndiff and name in ("q16", "k16", "v16"):
        d = np.abs(gg.astype(np.float32) - gu16.astype(np.float32))
        idx = np.argsort(-steps_metric(gg, gu16))[:4]
        print(f"   worst-step: golden={gg[idx]} gpu={gu16[idx]}")

print("\n== scores (pre-clamp GPU dump vs post-clamp golden) ==")
gs = np.fromfile(OUT + r"\gpu_scores.bin", dtype=np.float32).reshape(HEADS, TOK, TOK)
clipdiff = np.abs(np.clip(gs, -3, 3) - raw_clip)
d_raw = np.abs(gs - raw_clip)
print(f"raw   : max-abs={d_raw.max():.4f} mean-rel={(d_raw/np.maximum(np.abs(raw_clip),1e-6)).mean():.6f}"
      f"  frac|raw|>3: {(np.abs(gs)>3).mean()*100:.3f}%")
print(f"clipped: max-abs={clipdiff.max():.4f} mean-rel={(clipdiff/np.maximum(np.abs(raw_clip),1e-6)).mean():.6f}"
      f" max-rel={(clipdiff/np.maximum(np.abs(raw_clip),1e-6)).max():.4f}")

print("\n== fp32 tensors ==")
for name, g in [("branch", branch), ("ffn_out", ffn_out), ("proj", proj),
                ("merged", merged), ("attn_branch", attn_branch), ("block_raw", block_raw)]:
    gpu = np.fromfile(f"{OUT}\\gpu_{name}.bin", dtype=np.float32).reshape(g.shape)
    d = np.abs(gpu - g)
    denom = np.maximum(np.abs(g), 1e-6)
    rel = d / denom
    scale = np.abs(g).max()
    nbad = int((rel >= 0.02).sum())
    floor = scale * 1e-6
    relf = d / np.maximum(np.abs(g), floor)
    nbadf = int((relf >= 0.02).sum())
    print(f"{name}: max-abs={d.max():.4g} ({d.max()/scale:.3g} of scale) max-rel={rel.max():.3g}"
          f" (>{nbad}) mean-rel={rel.mean():.3g} scale={scale:.4g}"
          f" | floor=1e-6*scale: max-rel={relf.max():.4g} (>{nbadf})")

print("\n== float64 control: is strict max-rel<2% vs ANY fp32 GEMM achievable? ==")
proj64 = (ffn_hr.astype(np.float64) @ qkv.astype(np.float64)).astype(np.float32)
d64 = np.abs(proj64 - proj)
r64 = d64 / np.maximum(np.abs(proj), 1e-6)
print(f"numpy fp32 vs numpy fp64-rounded proj: max-abs={d64.max():.4g} "
      f"max-rel={r64.max():.3g} (>{int((r64>=0.02).sum())} elems >=2% on 1e-6 floor) "
      f"mean-rel={r64.mean():.3g}")
