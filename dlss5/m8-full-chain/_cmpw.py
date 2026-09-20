import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
out = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release\out\\"
p = "block5.layer0"
Gg = 2

gold = W[p + ".ffn_expand_weight"].reshape(Gg, 4, Gg, 32, 32).transpose(0, 2, 3, 1, 4).reshape(-1)
gpu = np.fromfile(out + "dbg5_expw.bin", dtype=np.float16).astype(np.float32).reshape(-1)
same = (gpu.astype(np.float16).view(np.uint16) == gold.astype(np.float16).view(np.uint16))
print("device expw == golden fold:", same.mean() * 100, "%")
if same.mean() < 1.0:
    diff = np.argwhere(~same)[:10].flatten()
    print("first diff idx:", diff.tolist())
    for i in diff[:5]:
        print(f"  idx {i}: gpu={gpu[i]} gold={gold[i]}")
