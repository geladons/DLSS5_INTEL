# SPDX-License-Identifier: Apache-2.0
import math
import numpy as np

def compare_head():
    path_i8 = "dlss5/m11d/build/Release/out/live_head_pilot0.bin"
    path_f16 = "dlss5/m11d/build/Release/out/live_head_fp16.bin"
    
    a = np.fromfile(path_i8, dtype=np.float32)
    b = np.fromfile(path_f16, dtype=np.float32)
    print(f"live_head comparison: {a.size} elements")
    
    diff = np.abs(a - b)
    max_d = float(np.max(diff))
    mean_d = float(np.mean(diff))
    mse = float(np.mean((a - b) ** 2))
    rmse = math.sqrt(mse)
    
    data_range = float(np.max(b) - np.min(b))
    psnr = 20.0 * math.log10(data_range / rmse) if rmse > 0 else 999.0
    corr = float(np.corrcoef(a.flatten(), b.flatten())[0, 1])
    
    print(f"  Max Diff:  {max_d:.6f}")
    print(f"  Mean Diff: {mean_d:.6f}")
    print(f"  MSE:       {mse:.8e}")
    print(f"  RMSE:      {rmse:.6f}")
    print(f"  Range:     {data_range:.4f}")
    print(f"  PSNR:      {psnr:.2f} dB")
    print(f"  Corr:      {corr:.6f}")

def compare_bmp():
    path_i8 = "dlss5/m11d/build/Release/out/selftest_pilot0.bmp"
    path_f16 = "dlss5/m11d/build/Release/out/selftest_fp16.bmp"
    
    with open(path_i8, "rb") as f:
        data_i8 = f.read()[54:] # skip 54-byte header
    with open(path_f16, "rb") as f:
        data_f16 = f.read()[54:]
        
    a = np.frombuffer(data_i8, dtype=np.uint8)
    b = np.frombuffer(data_f16, dtype=np.uint8)
    print(f"\nselftest_out.bmp pixel comparison: {a.size} bytes ({a.size // 4} pixels)")
    
    diff = np.abs(a.astype(np.int32) - b.astype(np.int32))
    max_d = int(np.max(diff))
    mean_d = float(np.mean(diff))
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    rmse = math.sqrt(mse)
    psnr = 20.0 * math.log10(255.0 / rmse) if rmse > 0 else 999.0
    
    exact_matches = int(np.sum(diff == 0))
    within_1 = int(np.sum(diff <= 1))
    
    print(f"  Max Pixel Diff (0-255):  {max_d}")
    print(f"  Mean Pixel Diff:         {mean_d:.4f}")
    print(f"  PSNR:                    {psnr:.2f} dB")
    print(f"  Exact Matching Bytes:    {exact_matches} / {a.size} ({100.0 * exact_matches / a.size:.2f}%)")
    print(f"  Within +/- 1 LSB:        {within_1} / {a.size} ({100.0 * within_1 / a.size:.2f}%)")

if __name__ == "__main__":
    compare_head()
    compare_bmp()
