from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
out = r"" + str(_REPO) + r"\dlss5\m8-full-chain\build\Release\out\\"
p = "block5.layer0"
Gg = 2
x = np.fromfile(out + "gpu_b4ds.f16", dtype=np.float16).astype(np.float32).reshape(288, 64)
x16 = G.half_rounded(x)
expansion = W[p + ".ffn_expand_weight"].reshape(Gg, 4, Gg, 32, 32).transpose(0, 2, 3, 1, 4).reshape(Gg, Gg * 32, 128)
h_gold = G.e4m3(G.gate_activation(x16 @ expansion[0]))
h_gpu = np.fromfile(out + "dbg5_h0.bin", dtype=np.float16).astype(np.float32).reshape(288, 128)

same = (h_gpu.astype(np.float16).view(np.uint16) == h_gold.astype(np.float16).view(np.uint16))
print("bitmatch", same.mean() * 100)
d = np.abs(h_gpu - h_gold)
print("maxabs", d.max(), "meanabs", d.mean())
# per-column match profile (128 cols)
colmatch = same.mean(axis=0)
print("colmatch min/max:", colmatch.min(), colmatch.max())
print("cols 0-15 :", (colmatch[:16] * 100).astype(int))
print("cols 32-47:", (colmatch[32:48] * 100).astype(int))
print("cols 64-79:", (colmatch[64:80] * 100).astype(int))
print("cols 96-111:", (colmatch[96:112] * 100).astype(int))
# row profile
rowmatch = same.mean(axis=1)
print("rowmatch: first 12:", (rowmatch[:12] * 100).astype(int))
print("row 288-12:", (rowmatch[-12:] * 100).astype(int))
# is gpu h0 maybe computed from a different input? correlate
print("gpu scale", np.abs(h_gpu).max(), "gold scale", np.abs(h_gold).max())
# check whether gpu matches golden computed with ih slices swapped in x
xswap = x.reshape(288, Gg, 32).copy()
xswap = xswap[:, ::-1, :].reshape(288, 64)
h_gs = G.e4m3(G.gate_activation(G.half_rounded(xswap) @ expansion[0]))
print("swap-ih bitmatch:", (h_gpu.astype(np.float16).view(np.uint16) == h_gs.astype(np.float16).view(np.uint16)).mean() * 100)
# check rows/cols swapped expansion
alt = W[p + ".ffn_expand_weight"].reshape(Gg, 4, Gg, 32, 32).transpose(0, 3, 2, 1, 4).reshape(Gg, 4 * 32, Gg * 32)
h_alt = G.e4m3(G.gate_activation(x16 @ alt[0].reshape(-1, 128)[:64]))  # rows= (br,r)? just try
print("alt shape", alt.shape)
