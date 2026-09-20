import json, struct
f = open(r"C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors", "rb")
hlen = struct.unpack("<Q", f.read(8))[0]
h = json.loads(f.read(hlen))
for k in h:
    if k == "__metadata__":
        continue
    if "block5." in k or "block4.layer0.weight0" in k or "block6." in k:
        print(k, h[k]["dtype"], h[k]["shape"])
