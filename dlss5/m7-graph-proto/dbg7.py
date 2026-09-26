from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import numpy as np
import struct, json
d = r"" + str(_REPO) + r"\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH = 288, 1024
x = np.fromfile(d + "x.bin", dtype=np.float32).reshape(TOK, CH)
branch_g = np.fromfile(d + "gpu_branch.bin", dtype=np.float32).reshape(TOK, CH)
ffn_g = np.fromfile(d + "gpu_ffn_out.bin", dtype=np.float32).reshape(TOK, CH)
ab = np.fromfile(d + "gpu_attn_branch.bin", dtype=np.float32).reshape(TOK, CH)
bo = np.fromfile(d + "gpu_block_raw.bin", dtype=np.float32).reshape(TOK, CH)
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
W = load(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
cos = W["block31.layer1.ffn_cos_skip"]
ac = W["block31.layer4.attn_cos_skip"]
ref = branch_g + x * cos
# hypothesis: oFF 2nd half = attn_branch + ffn*attn_cos (final residual wrote oFF)
h1 = ab + ref * ac
dd = np.abs(ffn_g[:, 512:] - h1[:, 512:]) / np.maximum(np.abs(h1[:, 512:]), 1e-6)
print("ffn[:,512:] vs attn_branch+ffn*attn_cos: mean-rel %.4g max %.4g" % (dd.mean(), dd.max()))
# or maybe oFF 2nd half = attn_branch + ffn_gpu*attn_cos
h2 = ab + ffn_g * ac
dd2 = np.abs(ffn_g[:, 512:] - h2[:, 512:]) / np.maximum(np.abs(h2[:, 512:]), 1e-6)
print("ffn[:,512:] vs attn_branch+ffn_gpu*attn_cos: mean-rel %.4g max %.4g" % (dd2.mean(), dd2.max()))
# check block_raw dump consistency
h3 = ab + ref * ac
dd3 = np.abs(bo - h3) / np.maximum(np.abs(h3), 1e-6)
print("block_raw vs attn_branch+ref*attn_cos: mean-rel %.4g" % dd3.mean())
# implied d at bad spots
bad = (np.abs(ffn_g - ref)/np.maximum(np.abs(ref),1e-6) > 0.01)
di = (ffn_g - branch_g)[bad] / x.reshape(-1)[bad.reshape(-1)]
print("implied d corr with cos[c]:", np.corrcoef(di, np.broadcast_to(cos, (TOK,CH)).reshape(-1)[bad.reshape(-1)])[0,1])
print("implied d corr with attn_cos[c]:", np.corrcoef(di, np.broadcast_to(ac, (TOK,CH)).reshape(-1)[bad.reshape(-1)])[0,1])
cs = np.broadcast_to(cos, (TOK,CH)); acs = np.broadcast_to(ac, (TOK,CH))
print("implied d corr with cos[c-512]:", np.corrcoef(di, np.roll(cs, 512, axis=1).reshape(-1)[bad.reshape(-1)])[0,1])
print("implied d corr with attn_cos[c-512]:", np.corrcoef(di, np.roll(acs, 512, axis=1).reshape(-1)[bad.reshape(-1)])[0,1])
