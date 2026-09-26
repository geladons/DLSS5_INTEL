#!/usr/bin/env python3
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys

import numpy as np

import golden as G

G.FRAG_SWIZZLE = np.arange(4096, dtype=np.intp)

od = r"build\Release\out"
st = r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors"
W = G.load_weights(st)
fc = W["block2.layer0.ffn_cos_skip"].reshape(-1).astype(np.float32)
g34 = np.fromfile(od + "\\g4_34.bin", dtype=np.float32).reshape(288, 32)
g35 = np.fromfile(od + "\\g4_35.bin", dtype=np.float32).reshape(288, 32)
x16 = np.fromfile(od + "\\dbg_b2x16.bin", dtype=np.float16).astype(np.float32).reshape(288, 32)

chg = np.abs(g35 - g34).max(axis=1) > 1e-9
idx = np.nonzero(chg)[0]
print("changed tokens:", idx.size, idx[:20])
for t in idx[:8]:
    ch = int(np.argmax(np.abs(g35[t] - g34[t])))
    print(f"tok {t} (y={t//12},x={t%12}) ch{ch}: g34={g34[t,ch]:.6f} g35={g35[t,ch]:.6f} "
          f"x16={x16[t,ch]:.6f} fc={fc[ch]:.4f} x16*fc={x16[t,ch]*fc[ch]:.6f} "
          f"want={g34[t,ch]+x16[t,ch]*fc[ch]:.6f}")
same = np.nonzero(~chg)[0]
print("unchanged tokens:", same.size, same[:10])
for t in same[:5]:
    print(f"tok {t}: g34={g34[t,0]:.6f} g35={g35[t,0]:.6f} x16*fc[0]={x16[t,0]*fc[0]:.6f}")
