import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
p = "block5.layer0"
raw = W[p + ".ffn_expand_weight"]              # [2,4,2,32,32] fp32 (from f16)
Gg = 2
s = raw.astype(np.float16).reshape(-1).view(np.uint16)   # bit reinterpret

d = np.zeros_like(s)
for oh in range(Gg):
    for ih in range(Gg):
        for r in range(32):
            for br in range(4):
                for c in range(32):
                    d[((oh * Gg + ih) * 32 + r) * 128 + br * 32 + c] = \
                        s[((oh * 4 + br) * Gg + ih) * 1024 + r * 32 + c]
cpu = d.view(np.float16).astype(np.float32).reshape(Gg, Gg * 32, 128)

gold = raw.reshape(Gg, 4, Gg, 32, 32).transpose(0, 2, 3, 1, 4).reshape(Gg, Gg * 32, 128)

eq = (cpu.astype(np.float16).view(np.uint16) == gold.astype(np.float16).view(np.uint16))
print("match:", eq.mean() * 100, "%")
diff = np.argwhere(~eq)
print("first diffs (oh, row, col):", diff[:8].tolist())
oh, row, col = diff[0]
ih, r = divmod(row, 32)
br, c = divmod(col, 32)
print("cpu[oh,ih,r,br,c] =", cpu[oh, row, col], " gold =", gold[oh, row, col])
print("raw[oh,br,ih,r,c] =", raw[oh, br, ih, r, c])
print("raw[oh,ih,br,r,c] =", raw[oh, ih, br, r, c])
# check the permuted guess
alt = raw.transpose(0, 3, 2, 1, 4).reshape(Gg, 4 * 32, Gg * 32)   # maybe rows=(br,r)?
print("alt shape", alt.shape)
