import numpy as np
import struct, json
d = r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m7-graph-proto\build\Release\out\\"
TOK, CH, HEADS, HDIM = 288, 1024, 32, 32
proj_g = np.fromfile(d + "gpu_proj.bin", dtype=np.float32).reshape(TOK, 3 * CH)
q16_g = np.fromfile(d + "gpu_q16.f16", dtype=np.float16).reshape(TOK, CH)
k16_g = np.fromfile(d + "gpu_k16.f16", dtype=np.float16).reshape(TOK, CH)
v16_g = np.fromfile(d + "gpu_v16.f16", dtype=np.float16).reshape(TOK, CH)
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
import math
qscale = W["block31.layer2.attn_scale"] * np.float32(math.sqrt(32))

def half_rounded(v):
    return np.asarray(v, np.float32).astype(np.float16).astype(np.float32)
def e4m3(x):
    s = np.asarray(x, np.float32)
    m = np.minimum(np.abs(s), np.float32(448.0))
    nm = np.maximum(m, np.float32(2.0**-6))
    eb = (nm.view(np.int32) >> 23) & 0xFF
    step = np.where(m < np.float32(2.0**-6), np.float32(2.0**-9), ((eb-3) << 23).view(np.float32))
    r = np.rint(m/step)*step
    return np.where(s < 0, -r, r).astype(np.float32)
def half_multiply(l, r): return (np.asarray(l, np.float32)*np.asarray(r, np.float32)).astype(np.float16).astype(np.float32)
def half_add(l, r): return (np.asarray(l, np.float32)+np.asarray(r, np.float32)).astype(np.float16).astype(np.float32)
def half_fma(l, r, a): return (np.asarray(l, np.float32)*np.asarray(r, np.float32)+np.asarray(a, np.float32)).astype(np.float16).astype(np.float32)
FLOOR = np.float32(0.00006198883056640625)
def cosine_normalize(value):
    half = half_rounded(value)
    partial = []
    for lane in range(4):
        lp = []
        for parity in range(2):
            ch = lane*2+parity
            first = half_fma(half[..., ch+8], half[..., ch+8], half_multiply(half[..., ch], half[..., ch]))
            second = half_fma(half[..., ch+24], half[..., ch+24], half_multiply(half[..., ch+16], half[..., ch+16]))
            lp.append(half_add(first, second))
        partial.append(np.stack(lp, axis=-1))
    pt = np.stack(partial, axis=-2)
    two = np.stack([half_add(pt[..., l, :], pt[..., l^2, :]) for l in range(4)], axis=-2)
    one = np.stack([half_add(two[..., l, :], two[..., l^1, :]) for l in range(4)], axis=-2)
    norm = half_add(one[..., 0, 0], one[..., 0, 1]).astype(np.float32)
    norm = np.maximum(norm, np.float32(np.float16(FLOOR)))
    recip = half_rounded(np.float32(1.0)/np.sqrt(norm))[..., None]
    return half_rounded(half*recip)
def cosine_publish(v, scale=None):
    n = cosine_normalize(v)
    if scale is not None:
        n = half_rounded(n * half_rounded(scale).reshape(scale.shape[0],1,1))
    return e4m3(n)

q = proj_g[:, :CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
k = proj_g[:, CH:2*CH].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
v = proj_g[:, 2*CH:].reshape(TOK, HEADS, HDIM).transpose(1, 0, 2)
qh = cosine_publish(q, qscale).astype(np.float16).transpose(1,0,2).reshape(TOK, CH)
kh = cosine_publish(k).astype(np.float16).transpose(1,0,2).reshape(TOK, CH)
vh = e4m3(v).astype(np.float16).transpose(1,0,2).reshape(TOK, CH)
for name, a, b in [("q16", qh, q16_g), ("k16", kh, k16_g), ("v16", vh, v16_g)]:
    same = (a.reshape(-1).view(np.uint16) == b.reshape(-1).view(np.uint16))
    da = np.abs(a.astype(np.float32) - b.astype(np.float32))
    print(name, "bitmatch %.4f%%" % (same.mean()*100), "max-abs %.4g" % da.max())
