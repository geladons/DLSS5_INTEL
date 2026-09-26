from pathlib import Path
_REPO = Path(__file__).resolve().parents[1]
import json, struct

p = r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors"
with open(p, "rb") as f:
    hlen = struct.unpack("<Q", f.read(8))[0]
    hdr = json.loads(f.read(hlen))

ESZ = {"F16": 2, "F32": 4}
blocks = {}
total = 0
for name, t in hdr.items():
    if name == "__metadata__":
        continue
    n = int(t["shape"][0] if False else name.split(".")[0].replace("block", ""))
    b = blocks.setdefault(n, [])
    nbytes = 1
    for d in t["shape"]:
        nbytes *= d
    nbytes *= ESZ[t["dtype"]]
    b.append((name, t["shape"], t["dtype"], nbytes))
    total += nbytes

grand = 0
for n in sorted(blocks):
    tot = sum(x[3] for x in blocks[n])
    grand += tot
    print(f"block{n}: {tot} bytes ({tot/1048576:.2f} MiB), {len(blocks[n])} tensors")
print(f"GRAND TOTAL: {grand} bytes = {grand/1048576:.2f} MiB (file data 291511674)")
print()
# canonical family check: print tensor names of any block whose name set deviates
import collections
sig = collections.defaultdict(list)
for n in sorted(blocks):
    names = tuple(sorted(x[0].split(".", 1)[1] for x in blocks[n]))
    sig[names].append(n)
for names, ns in sorted(sig.items(), key=lambda kv: kv[1][0]):
    print(f"blocks {ns}:")
    for x in sorted(blocks[ns[0]], key=lambda y: y[0]):
        print(f"   {x[0]} {x[1]} {x[2]}")
    print()
