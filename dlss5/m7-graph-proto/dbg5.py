import numpy as np
import struct, json
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH, HEADS, HDIM = 288, 1024, 32, 32
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch_g = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn_g = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)

def load(path):
    with open(path, "rb") as f:
        hlen = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(hlen))
        blob = f.read()
    out = {}
    for name, e in header.items():
        if name == "__metadata__":
            continue
        s0, s1 = e["data_offsets"]
        raw = blob[s0:s1]
        a = np.frombuffer(raw, dtype=np.float16 if e["dtype"] == "F16" else np.float32).astype(np.float32)
        out[name] = a.reshape(e["shape"])
    return out
W = load(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
cos = W["block31.layer1.ffn_cos_skip"]

# golden ffn_out from golden.py's own formula
ffn_ref = branch_g + x * cos
dd = np.abs(ffn_g - ffn_ref) / np.maximum(np.abs(ffn_ref), 1e-6)
print("gpu ffn_out vs branch+x*cos: mean-rel %.4g max-rel %.4g" % (dd.mean(), dd.max()))

# now full golden h pipeline to get golden branch
def half_rounded(v):
    return np.asarray(v, np.float32).astype(np.float16).astype(np.float32)
def e4m3(x):
    s = np.asarray(x, np.float32)
    m = np.minimum(np.abs(s), np.float32(448.0))
    nm = np.maximum(m, np.float32(2.0**-6))
    eb = (nm.view(np.int32) >> 23) & 0xFF
    step = np.where(m < np.float32(2.0**-6), np.float32(2.0**-9), ((eb-3) << 23).view(np.float32))
    r = np.rint(m/step)*step
    return np.where(s < 0, -r, r).astype(np.float32)
def gate(x):
    wide = half_rounded(x)
    cl = np.clip(wide, np.float32(-4), np.float32(4))
    lin = half_rounded(np.abs(cl)*np.float32(-0.055908203125)+np.float32(0.447265625))
    lin = half_rounded(lin*cl+np.float32(0.89453125))
    return half_rounded(wide*lin)
x_hr = half_rounded(x)
w0 = W["block31.layer0.weight"]
acc = x_hr @ w0
h = e4m3(gate(acc)).astype(np.float16)
w1 = W["block31.layer1.weight"]
branch_ref = h.astype(np.float32) @ w1
ffn_ref2 = branch_ref + x * cos
dd2 = np.abs(ffn_g - ffn_ref2) / np.maximum(np.abs(ffn_ref2), 1e-6)
print("gpu ffn_out vs golden-chain ffn_out: mean-rel %.4g max-rel %.4g" % (dd2.mean(), dd2.max()))
dd3 = np.abs(branch_g - branch_ref) / np.maximum(np.abs(branch_ref), 1e-6)
print("gpu branch vs golden branch: mean-rel %.4g max-rel %.4g" % (dd3.mean(), dd3.max()))
# h comparison
h_gpu = np.fromfile(d + "gpu_h.f16", dtype=np.float16).reshape(TOK, 4096)
same = (h.reshape(-1).view(np.uint16) == h_gpu.reshape(-1).view(np.uint16))
print("h bitmatch:", same.mean(), "n diff:", (~same).sum())
idx = np.argwhere(~same.reshape(TOK, 4096))
print("first diffs (t,c):", idx[:5].tolist())
for t, c in idx[:5]:
    print("  acc=%.6g golden_h=%.6g gpu_h=%.6g" % (acc[t, c], h[t, c], h_gpu[t, c]))
