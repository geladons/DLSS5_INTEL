from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
out = r"" + str(_REPO) + r"\dlss5\m8-full-chain\build\Release\out\\"

b30 = np.fromfile(out + "gpu_b30.f16", dtype=np.float16).astype(np.float32).reshape(288, 512)
bridge_gold = G.e4m3(b30 @ W["block30.layer4.weight"])          # [288, 1024]
bridge_gpu = np.fromfile(out + "dbg_bridge.bin", dtype=np.float16).astype(np.float32).reshape(288, 1024)
same = (bridge_gpu.astype(np.float16).view(np.uint16) == bridge_gold.astype(np.float16).view(np.uint16))
print("bridge bitmatch:", same.mean() * 100, "%  maxabs", np.abs(bridge_gpu - bridge_gold).max())
