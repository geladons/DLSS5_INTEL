from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
p = "block5.layer0"
exp = W[p + ".ffn_expand_weight"].reshape(-1)   # fp32 from loader (was f16)
Gg = 2
# CPU fold exactly as main.cpp staging
s = exp.astype(np.float16).astype(np.uint16)    # bit pattern preserved
d = np.zeros_like(s)
for oh in range(Gg):
    for ih in range(Gg):
        for r in range(32):
            for br in range(4):
                for c in range(32):
                    d[((oh * Gg + ih) * 32 + r) * 128 + br * 32 + c] = \
                        s[((oh * 4 + br) * Gg + ih) * 1024 + r * 32 + c]
cpu_fold = d.astype(np.uint16).view(np.float16).astype(np.float32).reshape(Gg, Gg * 32, 128)
golden_fold = W[p + ".ffn_expand_weight"].reshape(Gg, 4, Gg, 32, 32).transpose(0, 2, 3, 1, 4).reshape(Gg, Gg * 32, 128)
same = (cpu_fold.reshape(-1).astype(np.float16).view(np.uint16) == golden_fold.reshape(-1).astype(np.float16).view(np.uint16))
print("fold == golden transpose:", same.mean() * 100, "%")
