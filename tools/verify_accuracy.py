import os
import math
import numpy as np

base = r"c:\Users\<owner>\Desktop\DLSS5_INTEL"
live_head_path = os.path.join(base, r"dlss5\m11d\build\Release\out\live_head.bin")
golden_head_path = os.path.join(base, r"work\_m9b_cmp\golden_head.bin")

print("=== INT8 DLSSNR Numerical Verification ===")
if os.path.exists(live_head_path) and os.path.exists(golden_head_path):
    live = np.fromfile(live_head_path, dtype=np.float32)
    gold = np.fromfile(golden_head_path, dtype=np.float32)
    print(f"Elements: live={live.size}, gold={gold.size}")
    if live.size == gold.size:
        diff = np.abs(live - gold)
        max_diff = float(np.max(diff))
        mean_diff = float(np.mean(diff))
        mse = float(np.mean((live - gold) ** 2))
        data_range = float(np.max(gold) - np.min(gold))
        psnr = 20.0 * math.log10(data_range / math.sqrt(mse)) if mse > 0 else 999.0
        
        corr = float(np.corrcoef(live.flatten(), gold.flatten())[0, 1])

        print(f"Max Absolute Diff: {max_diff:.6f}")
        print(f"Mean Absolute Diff: {mean_diff:.6f}")
        print(f"MSE: {mse:.8e} (RMSE: {math.sqrt(mse):.6f})")
        print(f"Data Range: {data_range:.4f}")
        print(f"PSNR (vs torch golden): {psnr:.2f} dB")
        print(f"Pearson Correlation: {corr:.6f}")
        
        # Check against target
        if psnr >= 40.0:
            print(f"[PASS] Quality verified: PSNR {psnr:.2f} dB >= 40.0 dB golden threshold")
        else:
            print(f"[WARN] PSNR {psnr:.2f} dB below 40.0 dB threshold")
    else:
        print("Size mismatch between live and golden!")
else:
    print(f"Files not found: live={os.path.exists(live_head_path)}, gold={os.path.exists(golden_head_path)}")
