import os, sys
import numpy as np

def main():
    root = r"c:\Users\<owner>\Desktop\DLSS5_INTEL"
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, r"dlss5\m11d\build\Release\out")
    cmp_dir = os.path.join(root, r"work\_m9b_cmp")

    live_path = os.path.join(out_dir, "live_head.bin")
    golden_path = os.path.join(cmp_dir, "golden_head.bin")

    if not os.path.exists(live_path):
        print(f"Error: {live_path} not found")
        sys.exit(1)
    if not os.path.exists(golden_path):
        print(f"Error: {golden_path} not found")
        sys.exit(1)

    live_head = np.fromfile(live_path, dtype=np.float32).reshape(-1, 16)
    golden_head = np.fromfile(golden_path, dtype=np.float32).reshape(-1, 4)
    n = min(len(live_head), len(golden_head))

    baseline_exp = [0.008265, 0.007924, 0.008648]

    print("=" * 65)
    print("PRECISION VALIDATION: FP16 Accumulator vs PyTorch Golden Head")
    print(f"Elements compared: {n} pixels (first 3 color channels)")
    print("=" * 65)

    # Channel-wise stats
    diffs = []
    for ch in range(3):
        diff = np.abs(live_head[:n, ch] - golden_head[:n, ch])
        diffs.append(diff)
        meandiff = float(diff.mean())
        maxdiff = float(diff.max())
        mse = float(np.mean((live_head[:n, ch] - golden_head[:n, ch]) ** 2))
        data_range = float(golden_head[:n, ch].max() - golden_head[:n, ch].min())
        psnr_range = 10 * np.log10((data_range ** 2) / max(mse, 1e-12))
        psnr_1 = 10 * np.log10(1.0 / max(mse, 1e-12))
        delta_vs_baseline = meandiff - baseline_exp[ch]
        print(f"Channel {ch}:")
        print(f"  Mean Absolute Diff : {meandiff:.6f} (baseline: {baseline_exp[ch]:.6f}, delta: {delta_vs_baseline:+.6f})")
        print(f"  Max Absolute Diff  : {maxdiff:.6f}")
        print(f"  MSE                : {mse:.6e}")
        print(f"  PSNR (data_range={data_range:.3f}): {psnr_range:.2f} dB  (PSNR peak=1.0: {psnr_1:.2f} dB)")

    all_diff = np.concatenate([diffs[0], diffs[1], diffs[2]])
    overall_mean = float(all_diff.mean())
    overall_max = float(all_diff.max())
    overall_mse = float(np.mean((live_head[:n, :3] - golden_head[:n, :3]) ** 2))
    overall_range = float(golden_head[:n, :3].max() - golden_head[:n, :3].min())
    overall_psnr = 10 * np.log10((overall_range ** 2) / max(overall_mse, 1e-12))
    overall_psnr_1 = 10 * np.log10(1.0 / max(overall_mse, 1e-12))

    print("-" * 65)
    print("Overall (Channels 0..2):")
    print(f"  Overall Mean Abs Diff: {overall_mean:.6f} (baseline avg: {np.mean(baseline_exp):.6f})")
    print(f"  Overall Max Abs Diff : {overall_max:.6f}")
    print(f"  Overall MSE          : {overall_mse:.6e}")
    print(f"  Overall PSNR (range) : {overall_psnr:.2f} dB  (peak=1.0: {overall_psnr_1:.2f} dB)")
    print("=" * 65)

if __name__ == '__main__':
    main()
