import json, struct

path = r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"
with open(path, "rb") as f:
    hlen = struct.unpack("<Q", f.read(8))[0]
    header = json.loads(f.read(hlen))

ents = []
for name, e in header.items():
    if name == "__metadata__":
        continue
    esz = 4 if e["dtype"] == "F32" else 2
    numel = 1
    for s in e["shape"]:
        numel *= s
    blk = int(name.split(".")[0][5:])
    ents.append((blk, name, numel * esz))

ents.sort(key=lambda t: (t[0], t[1]))
o = 0
poff = {}
for blk, name, nbytes in ents:
    o = (o + 255) & ~255
    poff[name] = o
    o += nbytes

print("total", o)
for n in ["block0.layer0.input_adapter_weight", "block31.layer0.weight", "block39.layer0.conv_weight"]:
    print(n, poff[n])
# print first 8 of block31
print("--- block0 tensors ---")
for blk, name, nbytes in ents:
    if blk == 0:
        print(name, poff[name], nbytes)
