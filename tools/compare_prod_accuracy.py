# SPDX-License-Identifier: Apache-2.0
import os
import math
import numpy as np

prod_bmp = r"C:\Users\<owner>\Desktop\production\runtime\m11d\out\selftest_out.bmp"
fp16_bmp = r"C:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5\m11d\build\Release\out\selftest_fp16.bmp"
fused_bmp = r"C:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5\m11d\build\Release\out\selftest_fused_w1w2_p70.bmp"

def cmp_bmps(path_a, path_b, name_b):
    if not os.path.exists(path_a) or not os.path.exists(path_b):
        print(f"File missing: {path_a} or {path_b}")
        return
    a = np.fromfile(path_a, dtype=np.uint8)[54:] # skip BMP header
    b = np.fromfile(path_b, dtype=np.uint8)[54:]
    if a.size != b.size:
        print(f"Size mismatch: {a.size} vs {b.size}")
        return
    diff = np.abs(a.astype(np.float32) - b.astype(np.float32))
    mse = np.mean(diff ** 2)
    psnr = 20.0 * math.log10(255.0 / math.sqrt(mse)) if mse > 0 else 999.0
    exact = np.sum(diff == 0) / float(diff.size) * 100.0
    print(f"vs {name_b}: PSNR = {psnr:.2f} dB, exact = {exact:.2f}%, max_diff = {np.max(diff)}")

print("=== Production Bundle Selftest Accuracy ===")
cmp_bmps(prod_bmp, fp16_bmp, "FP16 Reference")
cmp_bmps(prod_bmp, fused_bmp, "Stage 14/15 Fused Reference")
