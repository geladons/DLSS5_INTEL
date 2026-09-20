import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
out = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release\out\\"
CH, HEADS, HDIM = 1024, 32, 32
idx = 31
p = f"block{idx}"

# b31 input = bridge output (bit-exact); recompute it here
b30 = np.fromfile(out + "gpu_b30.f16", dtype=np.float16).astype(np.float32).reshape(288, 512)
x = G.e4m3(b30 @ W["block30.layer4.weight"])

def f16(name, fn, g):
    gpu = np.fromfile(fn, dtype=np.float16).astype(np.float32).reshape(-1)
    g = np.ascontiguousarray(g).reshape(-1).astype(np.float16).astype(np.float32)
    same = (gpu.astype(np.float16).view(np.uint16) == g.astype(np.float16).view(np.uint16))
    d = np.abs(gpu - g)
    print(f"{name:<12} bitmatch {same.mean()*100:9.4f}%  maxabs {d.max():.6g}")

def f32(name, fn, g):
    gpu = np.fromfile(fn, dtype=np.float32).reshape(-1)
    g = np.ascontiguousarray(g).reshape(-1).astype(np.float32)
    d = np.abs(gpu - g)
    den = np.maximum(np.abs(g), 1e-6)
    print(f"{name:<12} maxabs {d.max():.6g}  meanrel {(d/den).mean():.6g}  eq {(d==0).mean()*100:.2f}%")

# --- golden stage computation (mirror _global_block) ---
x_hr = G.half_rounded(x)
acc = x_hr @ W[p + ".layer0.weight"]
h = G.e4m3(G.gate_activation(acc))
branch = h @ W[p + ".layer1.weight"]
ffn = branch + x * W[p + ".layer1.ffn_cos_skip"]
ffn_hr = G.half_rounded(ffn)
proj = ffn_hr @ W[p + ".layer2.qkv_weight"]
q = proj[:, 0:CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
k = proj[:, CH:2*CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
v = proj[:, 2*CH:3*CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
qscale = W[p + ".layer2.attn_scale"] * np.float32(np.sqrt(CH // HEADS))
q16 = G.cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)
k16 = G.cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
v16 = G.e4m3(np.ascontiguousarray(v)).astype(np.float16)
scores = np.clip(q16.astype(np.float32) @ k16.astype(np.float32).swapaxes(-1, -2), -3.0, 3.0)
probs = G.softmax(scores).astype(np.float16)
ctx = probs.astype(np.float32) @ v16.astype(np.float32)
merged = ctx.transpose(1, 0, 2).reshape(288, CH)
attended16 = G.e4m3(merged).astype(np.float16)
attn_branch = attended16.astype(np.float32) @ W[p + ".layer4.projection_weight"]
raw = attn_branch + ffn * W[p + ".layer4.attn_cos_skip"]

f16("in",  out+"dbg31_in.bin",  x)
f16("hg",  out+"dbg31_hg.bin",  h)
f32("ffn", out+"dbg31_ffn.bin", ffn)
f16("q",   out+"dbg31_q.bin",   q16.reshape(288, CH))
f16("k",   out+"dbg31_k.bin",   k16.reshape(288, CH))
f16("v",   out+"dbg31_v.bin",   v16.reshape(288, CH))
f16("at",  out+"dbg31_at.bin",  attended16)
f32("ab",  out+"dbg31_ab.bin",  attn_branch)
f16("pub", out+"dbg31_pub.bin", G.e4m3(raw))
