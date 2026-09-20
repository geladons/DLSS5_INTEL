import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
out = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release\out\\"

# b5 input: gpu b4ds (bit-exact vs golden)
x = np.fromfile(out + "gpu_b4ds.f16", dtype=np.float16).astype(np.float32).reshape(288, 64)

p = "block5.layer0"
Gg = 2
exp = W[p + ".ffn_expand_weight"]
prj = W[p + ".ffn_branch_projection_weight"]
expansion = exp.transpose(0, 2, 3, 1, 4).reshape(Gg, Gg * 32, 128)
projection = prj.reshape(Gg, 128, 32)
x16 = G.half_rounded(x)
outs = []
hs = []
for oh in range(Gg):
    h = G.e4m3(G.gate_activation(x16 @ expansion[oh]))
    hs.append(h)
    outs.append(G.e4m3(G.half_rounded(h) @ projection[oh]))

def cmpf16(name, fn, g):
    gpu = np.fromfile(fn, dtype=np.float16).reshape(-1)
    g = g.reshape(-1).astype(np.float16)
    same = (gpu.view(np.uint16) == g.view(np.uint16))
    d = np.abs(gpu.astype(np.float32) - g.astype(np.float32))
    print(f"{name:<14} bitmatch {same.mean()*100:9.4f}%  maxabs {d.max():.6g}")

cmpf16("h0(expand0)", out+"dbg5_h0.bin", hs[0])
cmpf16("cat(oG4)", out+"dbg5_cat.bin", np.concatenate(outs, axis=-1))
branch_cat = np.concatenate(outs, axis=-1)
branch = branch_cat @ W[p + ".ffn_output_projection_weight"]      # fp32
ffn_raw = branch + x * W[p + ".ffn_cos_skip"]                       # fp32 residual (kind5 c)
ffn = G.e4m3(ffn_raw)                                               # e4m3-valued (kind5 h)

def cmp(name, gpu, g, is16):
    gpu = gpu.reshape(-1)
    g = g.reshape(-1)
    if is16:
        same = (gpu.view(np.uint16) == g.astype(np.float16).view(np.uint16))
        d = np.abs(gpu.astype(np.float32) - g.astype(np.float32))
        print(f"{name:<14} bitmatch {same.mean()*100:9.4f}%  maxabs {d.max():.6g}  meanabs {d.mean():.6g}")
    else:
        d = np.abs(gpu - g)
        den = np.maximum(np.abs(g), 1e-6)
        print(f"{name:<14} maxabs {d.max():.6g}  meanrel {(d/den).mean():.6g}  bit-ish {(d==0).mean()*100:.2f}%")

cmp("ffn16(oG2)", np.fromfile(out+"dbg5_ffn16.bin", dtype=np.float16), ffn, True)
cmp("ffnraw(oG3)", np.fromfile(out+"dbg5_branch.bin", dtype=np.float32), ffn_raw, False)
print("golden branch scale:", np.abs(branch).max(), " x*cos scale:", np.abs(x*W[p+'.ffn_cos_skip']).max())

# window attention tail (origin (0,0), C=64, H=2, fam 0 weights under layer0)
C, H, heads = 64, 2, 2
oy, ox = 0, 0
wins, pt, pl = G._partition(ffn, oy, ox, C)
pr = wins @ W[p + ".qkv_weight"]
q = pr[..., :C].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
k = pr[..., C:2*C].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
v = pr[..., 2*C:].reshape(-1, 64, heads, 32).transpose(0, 2, 1, 3)
qn = G.cosine_normalize(q)
q16 = G.e4m3(G.half_rounded(qn * G.half_rounded(W[p+".attn_scale"]).reshape(1, heads, 1, 1)))
k16 = G.cosine_publish(np.ascontiguousarray(k))
v16 = G.e4m3(np.ascontiguousarray(v))
bias = W[p + ".attn_bias"]
scores = q16 @ k16.swapaxes(-1, -2) + bias[None]
probs = G.softmax(scores).astype(np.float32)
merged = (probs @ v16).transpose(0, 2, 1, 3).reshape(-1, 64, C)
att16 = G.e4m3(np.ascontiguousarray(merged)).astype(np.float32)
abw = att16 @ W[p + ".projection_weight"]            # window-order fp32 [W*64, C]
cmp("abw(oABW)", np.fromfile(out+"dbg5_abw.bin", dtype=np.float32), abw, False)
raw = G._reverse(abw, pt, pl, C) + ffn * W[p + ".attn_cos_skip"]
cmp("raw(oRAW)", np.fromfile(out+"dbg5_raw.bin", dtype=np.float32), raw, False)
cmp("pub(oG1)", np.fromfile(out+"dbg5_pub.bin", dtype=np.float16), G.e4m3(raw), True)

# also compare gpu raw vs gpu recompute of pub for self-consistency
