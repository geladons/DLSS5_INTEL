import numpy as np
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH = 288, 1024
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)
f16 = np.fromfile(d + "gpu_h.f16", dtype=np.float16).reshape(TOK, 4096).astype(np.float32)

# weights
import struct, json
def load(path):
    with open(path, "rb") as f:
        hlen = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(hlen))
        base = 8 + hlen
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
w1 = W["block31.layer1.weight"]
cos = W["block31.layer1.ffn_cos_skip"]

r = branch + x * cos
drel = np.abs(ffn - r) / np.maximum(np.abs(r), 1e-6)
print("ffn vs branch+x*cos: max-rel %.4g mean-rel %.4g max-abs %.4g" % (drel.max(), drel.mean(), np.abs(ffn - r).max()))

# branch vs h @ w1 (fp32)
br2 = f16 @ w1
drel2 = np.abs(branch - br2) / np.maximum(np.abs(br2), 1e-6)
print("branch vs h@w1: max-rel %.4g mean-rel %.4g max-abs %.4g" % (drel2.max(), drel2.mean(), np.abs(branch - br2).max()))

# ffn16 = to_half(ffn_out)?
f16b = np.fromfile(d + "gpu_f16.f16", dtype=np.float16) if False else None
# compare gpu ffn_out to golden-style residual using golden branch from h@w1
r2 = br2 + x * cos
drel3 = np.abs(ffn - r2) / np.maximum(np.abs(r2), 1e-6)
print("ffn vs (h@w1)+x*cos: max-rel %.4g mean-rel %.4g" % (drel3.max(), drel3.mean()))
