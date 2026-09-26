from pathlib import Path
_REPO = Path(__file__).resolve().parents[1]
import json, struct

p = r"" + str(_REPO) + r"\work\mlxw\dlssnr-logical.safetensors"
with open(p, "rb") as f:
    hlen = struct.unpack("<Q", f.read(8))[0]
    hdr = json.loads(f.read(hlen))

ESZ = {"F16": 2, "F32": 4}
def align(x, a=256): return (x + a - 1) // a * a

# order: block0..block70, within block by layer then name (deterministic)
entries = []
for name, t in hdr.items():
    if name == "__metadata__":
        continue
    blk = int(name.split(".")[0].replace("block", ""))
    nbytes = 1
    for d in t["shape"]:
        nbytes *= d
    nbytes *= ESZ[t["dtype"]]
    entries.append((blk, name, nbytes))

entries.sort(key=lambda e: (e[0], e[1]))

offset = 0
rows = []
per_block = {}
for blk, name, nbytes in entries:
    offset = align(offset)
    rows.append((blk, name, offset, nbytes))
    per_block.setdefault(blk, [offset, 0])
    per_block[blk][0] = min(per_block[blk][0], offset)
    per_block[blk][1] = offset + nbytes - per_block[blk][0]
    offset += nbytes

end = align(offset)
print(f"total aligned size: {end} bytes = {end/1048576:.2f} MiB (raw 278.01 MiB)")
print()
for blk in sorted(per_block):
    o, span = per_block[blk]
    print(f"block{blk}: offset={o:>10}  span={span:>9}  end={o+span:>10}")
# hot region = blocks 31..38
hot_o = per_block[31][0]
hot_e = per_block[38][0] + per_block[38][1]
print(f"\nHOT region blocks31-38: offset={hot_o} end={hot_e} size={(hot_e-hot_o)/1048576:.2f} MiB")
