import numpy as np
import struct, json
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH = 288, 1024
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)

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

q = (ffn - branch).reshape(-1)
print("corr with x*cos      :", np.corrcoef(q, (x * cos).reshape(-1))[0, 1])
print("corr with x          :", np.corrcoef(q, x.reshape(-1))[0, 1])
print("corr with cos(bcast) :", np.corrcoef(q, np.broadcast_to(cos, (TOK, CH)).reshape(-1))[0, 1])
# maybe ffn = branch + x*cos computed with WRONG x (e.g. x16 read as f32?) — try scale
s = q / np.maximum(np.abs((x * cos).reshape(-1)), 1e-30)
print("ratio (ffn-branch)/(x*cos): median %.6g  p5 %.6g  p95 %.6g" % (np.median(s), np.percentile(s, 5), np.percentile(s, 95)))
# is ffn maybe = branch + cos (missing x)?
print("corr with cos only   :", np.corrcoef(q, np.broadcast_to(cos, (TOK, CH)).reshape(-1))[0, 1])
# or branch*(1+cos)?
print("corr branch*cos      :", np.corrcoef(q, (branch * np.broadcast_to(cos, (TOK, CH))).reshape(-1))[0, 1])
