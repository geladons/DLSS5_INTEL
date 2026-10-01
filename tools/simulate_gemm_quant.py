# SPDX-License-Identifier: Apache-2.0
import os
import json
import numpy as np
import safetensors.numpy as sn

def quantize_int8_per_channel(W):
    """
    Per-channel weight quantization:
    W: shape (K, N)
    scale_w[j] = max(abs(W[:, j])) / 127.0
    W_q = np.clip(np.round(W / scale_w), -128, 127).astype(np.int8)
    """
    K, N = W.shape
    abs_max = np.max(np.abs(W), axis=0) # shape (N,)
    scale_w = abs_max / 127.0
    scale_w = np.where(scale_w == 0, 1.0, scale_w)
    W_q = np.clip(np.round(W / scale_w), -128, 127).astype(np.int8)
    return W_q, scale_w

def simulate_gemm(X, W, block_sizes=[32]):
    """
    X: (M, K) float32 (simulating fp16 input)
    W: (K, N) float32
    Returns metrics comparing Y_fp16 vs Y_int8 for static and dynamic block sizes.
    """
    M, K = X.shape
    K_w, N = W.shape
    assert K == K_w, f"K mismatch: {K} vs {K_w}"
    
    # FP16 reference computation
    X_f16 = X.astype(np.float16).astype(np.float32)
    W_f16 = W.astype(np.float16).astype(np.float32)
    Y_ref = X_f16 @ W_f16 # (M, N)
    
    # Quantize weights per-channel
    W_q, scale_w = quantize_int8_per_channel(W_f16) # (K, N) int8, (N,) float
    
    results = {}
    
    # -------------------------------------------------------------
    # (a) Static per-tensor activation scale
    # -------------------------------------------------------------
    max_x_static = np.max(np.abs(X_f16))
    scale_x_static = max_x_static / 127.0
    scale_x_static = 1.0 if scale_x_static == 0 else scale_x_static
    
    X_q_static = np.clip(np.round(X_f16 / scale_x_static), -128, 127).astype(np.int8)
    
    # INT8 GEMM (int32 accumulation)
    accum_static = X_q_static.astype(np.int32) @ W_q.astype(np.int32)
    
    # Dequantization: accum * scale_x * scale_w
    combined_scale_static = (scale_x_static * scale_w).astype(np.float32)
    Y_int8_static = accum_static.astype(np.float32) * combined_scale_static
    
    results["static_per_tensor"] = compute_metrics(Y_ref, Y_int8_static)
    results["static_per_tensor"]["scale_x_mean"] = float(scale_x_static)
    results["static_per_tensor"]["scale_x_max"] = float(scale_x_static)
    
    # -------------------------------------------------------------
    # (b) Dynamic token-block activation scale
    # -------------------------------------------------------------
    for bsize in block_sizes:
        assert M % bsize == 0, f"M={M} not divisible by block_size={bsize}"
        num_blocks = M // bsize
        
        # Reshape X into blocks: (num_blocks, bsize, K)
        X_reshaped = X_f16.reshape(num_blocks, bsize, K)
        # Block max abs: (num_blocks, 1, 1)
        block_max = np.max(np.abs(X_reshaped), axis=(1, 2), keepdims=True)
        scale_x_dyn = block_max / 127.0
        scale_x_dyn = np.where(scale_x_dyn == 0, 1.0, scale_x_dyn)
        
        X_q_dyn = np.clip(np.round(X_reshaped / scale_x_dyn), -128, 127).astype(np.int8)
        X_q_dyn = X_q_dyn.reshape(M, K)
        
        # GEMM
        accum_dyn = X_q_dyn.astype(np.int32) @ W_q.astype(np.int32) # (M, N)
        accum_dyn = accum_dyn.reshape(num_blocks, bsize, N)
        
        # Dequantize per block:
        # scale_x_dyn is (num_blocks, 1, 1), scale_w is (N,) -> (1, 1, N)
        scale_w_dyn = scale_w.reshape(1, 1, N)
        Y_int8_dyn = (accum_dyn.astype(np.float32) * (scale_x_dyn * scale_w_dyn)).reshape(M, N)
        
        results[f"dynamic_block_{bsize}"] = compute_metrics(Y_ref, Y_int8_dyn)
        results[f"dynamic_block_{bsize}"]["scale_x_mean"] = float(np.mean(scale_x_dyn))
        results[f"dynamic_block_{bsize}"]["scale_x_max"] = float(np.max(scale_x_dyn))
        results[f"dynamic_block_{bsize}"]["scale_x_min"] = float(np.min(scale_x_dyn))
        
    return results

def compute_metrics(Y_ref, Y_q):
    diff = Y_ref - Y_q
    mse = float(np.mean(diff ** 2))
    rmse = float(np.sqrt(mse))
    max_err = float(np.max(np.abs(diff)))
    
    ptp_val = float(np.ptp(Y_ref))
    max_abs = float(np.max(np.abs(Y_ref)))
    
    # PSNR based on dynamic range (peak-to-peak)
    psnr_ptp = 20.0 * np.log10(ptp_val / (rmse + 1e-12)) if rmse > 0 else 999.0
    # PSNR based on max abs
    psnr_max = 20.0 * np.log10(max_abs / (rmse + 1e-12)) if rmse > 0 else 999.0
    
    # Mean relative error
    # Guard against zero: relative to |Y_ref| + eps or normalized L1
    rel_err = np.abs(diff) / (np.abs(Y_ref) + 1e-4)
    mean_rel_err = float(np.mean(rel_err))
    
    # Relative Frobenius norm error
    norm_ref = np.linalg.norm(Y_ref)
    norm_diff = np.linalg.norm(diff)
    rel_l2 = float(norm_diff / (norm_ref + 1e-12))
    
    # Pearson correlation
    corr = float(np.corrcoef(Y_ref.flatten(), Y_q.flatten())[0, 1])
    
    return {
        "mse": mse,
        "rmse": rmse,
        "max_err": max_err,
        "psnr_ptp": psnr_ptp,
        "psnr_max": psnr_max,
        "mean_relative_error": mean_rel_err,
        "relative_l2_error": rel_l2,
        "correlation": corr
    }

def run_gemm_simulation_suite():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    weights_path = os.path.join(repo_root, "work", "mlxw", "dlssnr-logical.safetensors")
    
    print("Loading weights...")
    tensors = sn.load_file(weights_path)
    
    # Pick representative weights of shape (128, 32)
    # 1. block0.layer0.weight2
    # 2. block4.layer0.weight2
    # 3. block70.layer0.weight2
    # 4. ffn_branch_projection_weight slice
    weight_samples = {
        "block0_weight2": tensors["block0.layer0.weight2"],
        "block4_weight2": tensors["block4.layer0.weight2"],
        "block70_weight2": tensors["block70.layer0.weight2"],
        "block10_branch_prj": tensors["block10.layer0.ffn_branch_projection_weight"][0].reshape(128, 32)
    }
    
    shapes = [
        (138240, 128, 32),
        (552960, 128, 32)
    ]
    
    # Activation regimes:
    # 1. Standard Activation (smooth/Gaussian-like ~ N(0.04, 0.25^2) passed through quadratic gate + e4m3)
    # 2. Pathological Activation with Extreme Dynamic Range (standard activations + 0.1% outlier spikes up to 40.0-100.0 from HDR/specular reflections)
    # 3. Heavy-tailed / Student-t activation (kurtosis = high)
    
    np.random.seed(42)
    
    for M, K, N in shapes:
        print(f"\n==========================================================================================")
        print(f"EVALUATING GEMM SHAPE (M={M}, K={K}, N={N})")
        print(f"==========================================================================================")
        
        # 1. Generate base activations: standard DLSSNR gate distribution
        raw_pre = np.random.normal(0.04, 0.25, size=(M, K)).astype(np.float32)
        # Apply DLSSNR quadratic gate activation
        clamped = np.clip(raw_pre, -4.0, 4.0)
        linear = np.abs(clamped) * -0.055908203125 + 0.447265625
        linear = linear * clamped + 0.89453125
        X_standard = raw_pre * linear
        
        # 2. Pathological activations: extreme dynamic range
        # 0.05% of tokens have extreme outlier features (e.g. HDR light / specular reflection / attention spike)
        X_pathological = X_standard.copy()
        num_outliers = int(M * 0.0005) # ~69 tokens for 138k, ~276 tokens for 552k
        outlier_indices = np.random.choice(M, size=num_outliers, replace=False)
        # In these outlier tokens, spike activation to 20.0 .. 60.0
        outlier_values = np.random.uniform(20.0, 60.0, size=(num_outliers, K)) * np.random.choice([-1, 1], size=(num_outliers, K))
        X_pathological[outlier_indices, :] = outlier_values
        
        # 3. Severe Pathological: 0.1% outliers up to 100.0 (near max e4m3)
        X_severe = X_standard.copy()
        num_severe = int(M * 0.001)
        severe_indices = np.random.choice(M, size=num_severe, replace=False)
        severe_values = np.random.uniform(50.0, 150.0, size=(num_severe, K)) * np.random.choice([-1, 1], size=(num_severe, K))
        X_severe[severe_indices, :] = severe_values
        
        activation_regimes = [
            ("Standard DLSSNR Gated Activations (max ~0.85, std ~0.23)", X_standard),
            ("Pathological HDR/Specular Spikes (0.05% outliers in [20, 60], max ~60.0)", X_pathological),
            ("Severe Pathological HDR Spikes (0.1% outliers in [50, 150], max ~150.0)", X_severe)
        ]
        
        for reg_name, X_act in activation_regimes:
            print(f"\n>>> Activation Regime: {reg_name}")
            print(f"    Dynamic Range: min={np.min(X_act):.3f}, max={np.max(X_act):.3f}, mean={np.mean(X_act):.4f}, std={np.std(X_act):.4f}")
            
            for wname, W_weight in weight_samples.items():
                print(f"\n  -- Weight: {wname} --")
                sim_res = simulate_gemm(X_act, W_weight, block_sizes=[16, 32, 64])
                
                # Print comparison table
                print(f"  {'Method':<25} {'PSNR (ptp)':<12} {'PSNR (max)':<12} {'MeanRelErr':<12} {'Rel L2':<12} {'Max Error':<12} {'Corr':<10}")
                print(f"  {'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*12} {'-'*12} {'-'*10}")
                
                for method, m in sim_res.items():
                    print(f"  {method:<25} {m['psnr_ptp']:>10.2f} dB {m['psnr_max']:>10.2f} dB {m['mean_relative_error']:>11.5f} {m['relative_l2_error']:>11.5e} {m['max_err']:>11.5f} {m['correlation']:>9.6f}")

if __name__ == "__main__":
    run_gemm_simulation_suite()
