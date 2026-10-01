import struct
import numpy as np

# 1. Load INT8 binary weights pack
with open('work/mlxw/dlssnr-int8.bin', 'rb') as f:
    hdr = f.read(64)
    magic, ver, data_base, num_tensors = struct.unpack('<8sIII', hdr[:20])
    print(f"Header: magic={magic}, ver={ver}, data_base={data_base}, num_tensors={num_tensors}")
    
    tocs = {}
    for _ in range(num_tensors):
        toc = f.read(192)
        name = toc[:96].split(b'\x00')[0].decode('ascii')
        dtype, ndim = struct.unpack('<II', toc[96:104])
        shape = struct.unpack('<6q', toc[104:152])[:ndim]
        data_off, data_bytes, w_bytes, scale_off, scale_bytes = struct.unpack('<QQQQQ', toc[152:192])
        tocs[name] = {
            'dtype': dtype, 'shape': shape, 'data_off': data_off,
            'data_bytes': data_bytes, 'w_bytes': w_bytes,
            'scale_off': scale_off, 'scale_bytes': scale_bytes
        }

# Inspect block0.layer0.weight1
w1_info = tocs['block0.layer0.weight1']
print(f"\nblock0.layer0.weight1 info: {w1_info}")

with open('work/mlxw/dlssnr-int8.bin', 'rb') as f:
    f.seek(data_base + w1_info['data_off'])
    w1_bytes = f.read(w1_info['w_bytes'])
    W_q = np.frombuffer(w1_bytes, dtype=np.int8).reshape(w1_info['shape'])
    
    f.seek(data_base + w1_info['data_off'] + w1_info['scale_off'])
    scale_w_bytes = f.read(w1_info['shape'][1] * 4)
    scale_w = np.frombuffer(scale_w_bytes, dtype=np.float32)

print(f"W_q shape={W_q.shape}, min={W_q.min()}, max={W_q.max()}")
print(f"scale_w shape={scale_w.shape}, min={scale_w.min()}, max={scale_w.max()}")
print(f"scale_w has NaN/Inf: {np.isnan(scale_w).sum()}/{np.isinf(scale_w).sum()}")
print(f"scale_w has zeros: {(scale_w == 0).sum()}")

# 2. Load live_ada.bin
ada = np.fromfile('dlss5/m11d/build/Release/out/live_ada.bin', dtype=np.float32)
print(f"\nlive_ada shape={ada.shape}, min={ada.min()}, max={ada.max()}")
print(f"live_ada has NaN/Inf: {np.isnan(ada).sum()}/{np.isinf(ada).sum()}")

# 3. Simulate dEw(kind=2): fp32 -> fp16
ada_f16 = ada.astype(np.float16)
print(f"ada_f16 min={ada_f16.min()}, max={ada_f16.max()}, has NaN: {np.isnan(ada_f16).sum()}")

# Reshape to [TOKS, 32]
M = len(ada) // 32
X = ada_f16.reshape(M, 32).astype(np.float32)
print(f"X shape={X.shape}")

# 4. Simulate quant_act with block 32
num_blocks = (M + 31) // 32
scales_a = np.zeros(num_blocks, dtype=np.float32)
X_q = np.zeros_like(X, dtype=np.int8)

for b in range(num_blocks):
    r_start = b * 32
    r_end = min(r_start + 32, M)
    block = X[r_start:r_end, :]
    b_max = float(np.max(np.abs(block)))
    sc = b_max / 127.0 if b_max > 0 else 1.0
    scales_a[b] = sc
    X_q[r_start:r_end, :] = np.clip(np.round(block / sc), -127, 127).astype(np.int8)

print(f"\nscales_a min={scales_a.min()}, max={scales_a.max()}, has NaN/Inf: {np.isnan(scales_a).sum()}/{np.isinf(scales_a).sum()}")
print(f"X_q min={X_q.min()}, max={X_q.max()}")

# 5. Simulate GEMM
# Out[b*32..(b+1)*32, :] = (X_q * W_q) * (scales_a[b] * scale_w)
out_sim = np.zeros((M, 128), dtype=np.float32)
for b in range(min(num_blocks, 100)): # test first 100 blocks
    r_start = b * 32
    r_end = min(r_start + 32, M)
    acc = np.matmul(X_q[r_start:r_end, :].astype(np.int32), W_q.astype(np.int32))
    factor = scales_a[b] * scale_w # [128]
    val = acc.astype(np.float32) * factor
    out_sim[r_start:r_end, :] = val

print(f"\nout_sim (first 100 blocks): min={out_sim[:3200].min():.4f}, max={out_sim[:3200].max():.4f}, has NaN: {np.isnan(out_sim[:3200]).sum()}")
