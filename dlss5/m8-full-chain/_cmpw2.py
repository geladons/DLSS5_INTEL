import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden as G

W = G.load_weights(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
out = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release\out\\"
p = "block5.layer0"
Gg = 2
raw = W[p + ".ffn_expand_weight"].reshape(Gg, 4, Gg, 32, 32)   # [oh, br, ih, r, c]
gpu = np.fromfile(out + "dbg5_expw.bin", dtype=np.float16).astype(np.float32).reshape(-1)
gold = raw.transpose(0, 2, 3, 1, 4).reshape(-1)

# for the first 6 gpu rows (64 f16), find each row's best-matching location in raw flat
rawflat = raw.reshape(-1)
def find_row(row):
    best, bi = -1, -1
    for off in range(0, len(rawflat) - 32 + 1, 32):
        m = (row == rawflat[off:off+32]).mean()
        if m > best:
            best, bi = m, off
    return bi // 32, best

for row in range(6):
    blk, score = find_row(gpu[row*32:(row+1)*32])
    oh, br, ih, r = np.unravel_index(blk*32, raw.shape)[0], np.unravel_index(blk*32, raw.shape)[1], np.unravel_index(blk*32, raw.shape)[2], np.unravel_index(blk*32, raw.shape)[3]
    print(f"gpu row {row:2d} (gold row {row}: (ih={row//32}, r={row%32})) -> raw block flat {blk*32} = (oh={oh}, br={br}, ih={ih}, r={r})  match {score*100:.0f}%")
