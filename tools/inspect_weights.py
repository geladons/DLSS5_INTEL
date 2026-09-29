import json
import struct
import os

repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
safetensors_path = os.path.join(repo_root, "work", "mlxw", "dlssnr-logical.safetensors")

with open(safetensors_path, "rb") as f:
    hlen = struct.unpack("<Q", f.read(8))[0]
    hdr = json.loads(f.read(hlen))

print(f"Total keys in header: {len(hdr)}")

tensor_types = {}
for k, v in hdr.items():
    if k == "__metadata__":
        continue
    dtype = v.get("dtype")
    shape = tuple(v.get("shape", []))
    suffix = k.split(".")[-1]
    tensor_types.setdefault(suffix, []).append((k, shape, dtype))

print("\n--- Tensor Suffixes and Sample Shapes ---")
for sfx in sorted(tensor_types.keys()):
    items = tensor_types[sfx]
    shapes = sorted(list(set(item[1] for item in items)))
    dtypes = sorted(list(set(item[2] for item in items)))
    print(f"{sfx:<35} count={len(items):<3} dtypes={dtypes} shapes={shapes}")
