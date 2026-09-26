from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
out = r"" + str(_REPO) + r"\dlss5\m8-full-chain\build\Release\out\\"
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
q16 = G.cosine_publish(np.ascontiguousarray(q), qscale).astype(np.float16)
k16 = G.cosine_publish(np.ascontiguousarray(k)).astype(np.float16)
v16 = G.e4m3(np.ascontiguousarray(v)).astype(np.float16)
scores = np.clip(q16.astype(np.float32) @ k16.astype(np.float32).swapaxes(-1, -2), -3.0, 3.0)
probs = G.softmax(scores).astype(np.float16)
ctx = probs.astype(np.float32) @ v16.astype(np.float32)
merged = ctx.transpose(1, 0, 2).reshape(288, CH)
attended16 = G.e4m3(merged).astype(np.float16)
attn_branch = attended16.astype(np.float32) @ W[p+".layer4.projection_weight"]
raw = attn_branch + ffn * W[p+".layer4.attn_cos_skip"]

gpu_pr = np.fromfile(out+"dbg31_pr.bin", dtype=np.float16).reshape(HEADS, 288, 288)
same = (gpu_pr.view(np.uint16) == probs.view(np.uint16))
print("probs bitmatch:", same.mean()*100, "%  maxabs", np.abs(gpu_pr.astype(np.float32)-probs.astype(np.float32)).max())

gpu_at = np.fromfile(out+"dbg31_at.bin", dtype=np.float16).astype(np.float32).reshape(288, CH)
g_at = attended16.astype(np.float32).reshape(288, CH)
same = (gpu_at.astype(np.float16).view(np.uint16) == g_at.astype(np.float16).view(np.uint16))
print("attended bitmatch:", same.mean()*100, "%  maxabs", np.abs(gpu_at-g_at).max())

gpu_pub = np.fromfile(out+"dbg31_pub.bin", dtype=np.float16).astype(np.float32).reshape(288, CH)
g_pub = G.e4m3(raw).astype(np.float16).astype(np.float32).reshape(288, CH)
same = (gpu_pub.astype(np.float16).view(np.uint16) == g_pub.astype(np.float16).view(np.uint16))
print("pub bitmatch:", same.mean()*100, "%  maxabs", np.abs(gpu_pub-g_pub).max())
