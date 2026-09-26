from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import json, struct
p = r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors"
with open(p, "rb") as f:
    hlen = struct.unpack("<Q", f.read(8))[0]
    hdr = json.loads(f.read(hlen))
for name in ["block0.layer0.attn_bias", "block0.layer0.attn_scale",
             "block0.layer0.qkv_weight", "block0.layer0.projection_weight",
             "block0.layer0.ffn_cos_skip", "block0.layer0.attn_cos_skip",
             "block0.layer0.weight1", "block0.layer0.weight2"]:
    e = hdr.get(name)
    if e is None:
        print(name, "MISSING")
    else:
        s0, s1 = e["data_offsets"]
        print(name, e["dtype"], e["shape"], (s1 - s0), "bytes")
# any block0 attn-ish tensors
print("--- block0 tensors:")
for k in sorted(hdr):
    if k.startswith("block0."):
        print(" ", k, hdr[k]["shape"])
