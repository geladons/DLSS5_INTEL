from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import numpy as np
import struct, json
d = r"" + str(_REPO) + r"\dlss5\m7-graph-proto\build\Release\out\\"
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
W = load(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors")
cos = W["block31.layer1.ffn_cos_skip"]

q = ffn - branch
r = q / np.where(np.abs(x) > 1e-3, x, np.nan)   # per-element implied d
ch_med = np.nanmedian(r, axis=0)                # (CH,) implied per-channel d
print("implied d: mean %.6g std %.6g | cos: mean %.6g std %.6g" % (ch_med.mean(), ch_med.std(), cos.mean(), cos.std()))
print("corr(implied d, cos):", np.corrcoef(ch_med, cos)[0, 1])
# per-row instead?
row_med = np.nanmedian(r, axis=1)
print("implied d per-row std: %.6g (per-channel std above)" % row_med.std())
# Maybe ffn = branch + x*cos but with x from a DIFFERENT layout (transposed)?
xt = x.T.copy()
m = x != 0
r2 = q[m] / xt.reshape(-1)[: q.size][m.reshape(-1)]
print("vs x transposed: corr", np.corrcoef(q.reshape(-1), (xt * cos[:, None]).reshape(-1))[0, 1] if False else np.corrcoef(q.reshape(-1), np.broadcast_to(cos[:, None], (CH, TOK)).T.reshape(-1))[0, 1])
# what about ffn = branch + x * cos where cos read as f16 bit-reinterp?
cos16 = W["block31.layer1.ffn_cos_skip"].astype(np.float16).astype(np.float32)
print("corr with x*cos16:", np.corrcoef(q.reshape(-1), (x * cos16).reshape(-1))[0, 1])
# maybe d = attn_cos (block31.layer4.attn_cos_skip)?
print("corr with x*attn_cos:", np.corrcoef(q.reshape(-1), (x * W["block31.layer4.attn_cos_skip"]).reshape(-1))[0, 1])
# or maybe ffn = branch + x*cos + extra noise from overwriting: check a few raw values
print("sample q[0,:6]:", q[0, :6])
print("sample x*cos[0,:6]:", (x * np.broadcast_to(cos, (TOK, CH)))[0, :6])
print("q stats: abs mean %.4g p99 %.4g max %.4g" % (np.abs(q).mean(), np.percentile(np.abs(q), 99), np.abs(q).max()))
