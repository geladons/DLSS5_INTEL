import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
out = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release\out\\"
CH, HEADS, HDIM = 1024, 32, 32
p = "block31"

b30 = np.fromfile(out + "gpu_b30.f16", dtype=np.float16).astype(np.float32).reshape(288, 512)
x = G.e4m3(b30 @ W["block30.layer4.weight"])
ffn = (G.e4m3(G.gate_activation(G.half_rounded(x) @ W[p+".layer0.weight"])) @ W[p+".layer1.weight"]) + x * W[p+".layer1.ffn_cos_skip"]
proj = G.half_rounded(ffn) @ W[p+".layer2.qkv_weight"]
q = proj[:, 0:CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
k = proj[:, CH:2*CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
v = proj[:, 2*CH:3*CH].reshape(288, HEADS, HDIM).transpose(1, 0, 2)
qscale = W[p + ".layer2.attn_scale"] * np.float32(np.sqrt(CH // HEADS))
q16 = G.cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)   # (H,T,32)
k16 = G.cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
v16 = G.e4m3(np.ascontiguousarray(v)).astype(np.float16)

def cmp16(name, fn, g_h_t_d, order):
    gpu = np.fromfile(fn, dtype=np.float16).astype(np.float32).reshape(288, 1024)
    if order == "tmajor":   # [t, head*32+d]
        g = np.ascontiguousarray(g_h_t_d.transpose(1, 0, 2)).reshape(288, 1024)
    else:                    # scramble as reshape(288,1024) of (H,T,32)
        g = g_h_t_d.reshape(288, 1024)
    same = (gpu.astype(np.float16).view(np.uint16) == g.astype(np.float16).view(np.uint16))
    print(f"{name} {order}: bitmatch {same.mean()*100:.4f}%  maxabs {np.abs(gpu-g).max():.6g}")

cmp16("q", out+"dbg31_q.bin", q16, "tmajor")
cmp16("q", out+"dbg31_q.bin", q16, "scramble")
cmp16("k", out+"dbg31_k.bin", k16, "tmajor")
cmp16("k", out+"dbg31_k.bin", k16, "scramble")
cmp16("v", out+"dbg31_v.bin", v16, "tmajor")
cmp16("v", out+"dbg31_v.bin", v16, "scramble")

# also: maybe GPU v = e4m3 of something else? check against raw proj V fp32 e4m3
vraw = G.e4m3(np.ascontiguousarray(v)).astype(np.float16)
# what if GPU skipped e4m3 and wrote half_round? or wrote fp16 of raw?
v_hr = G.half_rounded(np.ascontiguousarray(v)).astype(np.float16)
gpu = np.fromfile(out+"dbg31_v.bin", dtype=np.float16).astype(np.float32).reshape(288, 1024)
g_hr = np.ascontiguousarray(v_hr.transpose(1,0,2)).reshape(288,1024)
print("v as half_round tmajor:", (gpu.astype(np.float16).view(np.uint16) == g_hr.astype(np.float16).view(np.uint16)).mean()*100)
