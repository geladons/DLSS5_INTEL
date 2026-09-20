import numpy as np
import struct, json
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH = 288, 1024
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch_g = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn_g = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)
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
cos = W["block31.layer1.ffn_cos_skip"]; ac = W["block31.layer4.attn_cos_skip"]
q = (ffn_g - branch_g)[0]  # row 0
xc = x[0]
di = q / xc
for c in range(508, 524):
    print("c=%d d_implied=%.8f cos[c]=%.8f ac[c]=%.8f ac[c-512]=%.8f cos[c-512]=%.8f" %
          (c, di[c], cos[c], ac[c], ac[c-512] if c>=512 else float('nan'), cos[c-512] if c>=512 else float('nan')))
# is q[512:] == x[0,512:]*ac[0:512]?
print("q[512:516] =", q[512:516])
print("x*ac[0:4]  =", (xc[512:516]*ac[:4]))
print("q[:4]      =", q[:4])
print("x*cos[:4]  =", (xc[:4]*cos[:4]))
