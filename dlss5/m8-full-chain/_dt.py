from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import json, struct
f = open(r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors", "rb")
hlen = struct.unpack("<Q", f.read(8))[0]
h = json.loads(f.read(hlen))
for k in h:
    if k == "__metadata__":
        continue
    if "block5." in k or "block4.layer0.weight0" in k or "block6." in k:
        print(k, h[k]["dtype"], h[k]["shape"])
