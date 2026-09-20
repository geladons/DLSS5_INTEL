import numpy as np
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH = 288, 1024
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch_g = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn_g = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)
import struct, json
def load(path):
    with open(path, "rb") as f:
        hlen = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(hlen)); blob = f.read()
    out = {}
    for name, e in header.items():
        if name == "__metadata__": continue
        s0, s1 = e["data_offsets"]; raw = blob[s0:s1]
        a = np.frombuffer(raw, dtype=np.float16 if e["dtype"]=="F16" else np.float32).astype(np.float32)
        out[name] = a.reshape(e["shape"])
    return out
W = load(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors")
cos = W["block31.layer1.ffn_cos_skip"]
ref = branch_g + x * cos
dd = np.abs(ffn_g - ref) / np.maximum(np.abs(ref), 1e-6)
bad = dd > 0.01
print("bad count:", bad.sum(), "/", bad.size)
badrows = bad.any(axis=1); badcols = bad.any(axis=0)
print("bad rows:", np.where(badrows)[0][:50].tolist(), "... total", badrows.sum())
print("bad cols:", np.where(badcols)[0][:50].tolist(), "... total", badcols.sum())
# distribution of bad elements per row
r = np.argwhere(bad)
if len(r):
    import collections
    rows = collections.Counter(r[:,0].tolist())
    print("bad-per-row sample:", list(rows.items())[:10])
    # what does ffn hold at bad spots vs ref?
    t, c = r[0]
    print("example bad: t=%d c=%d gpu=%.6g ref=%.6g branch=%.6g x=%.6g cos=%.6g" % (t, c, ffn_g[t,c], ref[t,c], branch_g[t,c], x[t,c], cos[c]))
    # is gpu ffn == ref for the SAME row but treating x as x16? or ffn==branch?
    print("gpu vs branch at bad spots: max abs", np.abs(ffn_g[bad]-branch_g[bad]).max())
    print("gpu vs x*cos at bad spots: max abs", np.abs(ffn_g[bad]).max())
