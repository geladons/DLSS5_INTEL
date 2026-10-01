import struct

with open('work/mlxw/dlssnr-int8.bin', 'rb') as f:
    hdr = f.read(64)
    magic, ver, hdr_sz, num_tensors = struct.unpack('<8sIII', hdr[:20])
    print(f'magic={magic}, ver={ver}, hdr_sz={hdr_sz}, num_tensors={num_tensors}')
    q_names = []
    # Int8TocEntry: 96 (name) + 4 (dtype) + 4 (ndim) + 48 (shape: 6*8) + 8 (data_offset) + 8 (data_bytes) + 8 (w_bytes) + 8 (scale_offset) + 8 (scale_bytes) = 192 bytes
    for _ in range(num_tensors):
        toc = f.read(192)
        name = toc[:96].split(b'\x00')[0].decode('ascii')
        dtype, ndim = struct.unpack('<II', toc[96:104])
        shape = struct.unpack('<6q', toc[104:152])
        if dtype == 2:
            q_names.append((name, shape[:ndim]))

print(f'Total INT8 quantized tensors: {len(q_names)}')
print('First 20 quantized tensors:')
for name, sh in q_names[:20]:
    print(f'  {name:<48} {sh}')
