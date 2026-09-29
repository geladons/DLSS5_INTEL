import os
import json
import struct
import numpy as np
import safetensors.numpy as sn

def analyze_all_weights():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    weights_path = os.path.join(repo_root, "work", "mlxw", "dlssnr-logical.safetensors")
    
    print(f"Loading weights from {weights_path}...")
    tensors = sn.load_file(weights_path)
    print(f"Loaded {len(tensors)} tensors.")
    
    gemm_2d_weights = {}
    gemm_3d_weights = {}
    other_tensors = {}
    
    for name, arr in tensors.items():
        if arr.ndim == 2:
            gemm_2d_weights[name] = arr
        elif arr.ndim == 3:
            if "attn_bias" in name:
                other_tensors[name] = arr
            else:
                gemm_3d_weights[name] = arr
        else:
            other_tensors[name] = arr
            
    print(f"Identified {len(gemm_2d_weights)} 2D GEMM weight tensors.")
    print(f"Identified {len(gemm_3d_weights)} 3D GEMM weight tensors.")
    print(f"Other tensors: {len(other_tensors)} (including attn_bias, skips, scales, etc.)")
    
    # Analyze 2D weights
    results_2d = []
    all_scales_2d = []
    
    total_elements_2d = 0
    total_sq_err_2d = 0.0
    global_max_err_2d = 0.0
    
    by_category_2d = {}
    
    for name, W_f16 in sorted(gemm_2d_weights.items()):
        W = W_f16.astype(np.float32)
        K, N = W.shape
        
        # Per-channel absolute maximums along columns (axis 0)
        # scale_w[j] = max(abs(W[:, j])) / 127.0
        abs_max_col = np.max(np.abs(W), axis=0)
        scale_w = abs_max_col / 127.0
        # Prevent div by zero if any column is all zeros
        scale_w_safe = np.where(scale_w == 0, 1.0, scale_w)
        
        # Quantize weights to INT8
        W_q = np.clip(np.round(W / scale_w_safe), -128, 127).astype(np.int8)
        
        # Reconstruction
        W_rec = W_q.astype(np.float32) * scale_w_safe
        
        err = W - W_rec
        sq_err = err ** 2
        mse = float(np.mean(sq_err))
        max_err = float(np.max(np.abs(err)))
        
        total_elements_2d += W.size
        total_sq_err_2d += float(np.sum(sq_err))
        if max_err > global_max_err_2d:
            global_max_err_2d = max_err
            
        all_scales_2d.extend(scale_w.tolist())
        
        # Categorize
        cat = name.split(".")[-1]
        by_category_2d.setdefault(cat, []).append({
            "name": name,
            "shape": (K, N),
            "mse": mse,
            "max_err": max_err,
            "scales": scale_w,
            "min_scale": float(np.min(scale_w)),
            "max_scale": float(np.max(scale_w)),
            "mean_scale": float(np.mean(scale_w)),
            "numel": W.size
        })
        
        results_2d.append({
            "name": name,
            "shape": (K, N),
            "mse": mse,
            "max_err": max_err,
            "scale_min": float(np.min(scale_w)),
            "scale_max": float(np.max(scale_w)),
            "scale_mean": float(np.mean(scale_w))
        })
        
    global_mse_2d = total_sq_err_2d / total_elements_2d
    all_scales_2d = np.array(all_scales_2d, dtype=np.float32)
    
    print("\n=================== 2D GEMM WEIGHT QUANTIZATION RESULTS ===================")
    print(f"Total 2D tensors evaluated: {len(results_2d)}")
    print(f"Total elements: {total_elements_2d:,} ({total_elements_2d * 2 / 1024 / 1024:.2f} MB in FP16, {total_elements_2d / 1024 / 1024:.2f} MB in INT8)")
    print(f"Global Weighted MSE: {global_mse_2d:.8e} (RMSE: {np.sqrt(global_mse_2d):.6f})")
    print(f"Global Max Absolute Error: {global_max_err_2d:.8f}")
    print(f"Scale Distribution (N={len(all_scales_2d):,} channels):")
    print(f"  Min:    {np.min(all_scales_2d):.8f}")
    print(f"  1%:     {np.percentile(all_scales_2d, 1):.8f}")
    print(f"  5%:     {np.percentile(all_scales_2d, 5):.8f}")
    print(f"  25%:    {np.percentile(all_scales_2d, 25):.8f}")
    print(f"  Median: {np.median(all_scales_2d):.8f}")
    print(f"  Mean:   {np.mean(all_scales_2d):.8f}")
    print(f"  75%:    {np.percentile(all_scales_2d, 75):.8f}")
    print(f"  95%:    {np.percentile(all_scales_2d, 95):.8f}")
    print(f"  99%:    {np.percentile(all_scales_2d, 99):.8f}")
    print(f"  Max:    {np.max(all_scales_2d):.8f}")
    print(f"  Std:    {np.std(all_scales_2d):.8f}")
    
    print("\n--- Breakdown by 2D Tensor Category ---")
    for cat, items in sorted(by_category_2d.items()):
        cat_numel = sum(it["numel"] for it in items)
        cat_sq_err = sum(it["mse"] * it["numel"] for it in items)
        cat_mse = cat_sq_err / cat_numel
        cat_max_err = max(it["max_err"] for it in items)
        cat_scales = np.concatenate([it["scales"] for it in items])
        shapes = sorted(list(set(str(it["shape"]) for it in items)))
        print(f"{cat:<30} count={len(items):<3} shapes={shapes} numel={cat_numel:>10,} MSE={cat_mse:.3e} MaxErr={cat_max_err:.6f} ScaleMean={np.mean(cat_scales):.6f} ScaleMax={np.max(cat_scales):.6f}")

    # Analyze 3D weights
    print("\n=================== 3D GEMM WEIGHT QUANTIZATION RESULTS ===================")
    results_3d = []
    all_scales_3d = []
    total_elements_3d = 0
    total_sq_err_3d = 0.0
    global_max_err_3d = 0.0
    by_category_3d = {}
    
    for name, W_f16 in sorted(gemm_3d_weights.items()):
        W = W_f16.astype(np.float32)
        B, K, N = W.shape
        
        # Per-channel scale: for each batch b and channel j: scale_w[b, j] = max(abs(W[b, :, j])) / 127.0
        abs_max = np.max(np.abs(W), axis=1) # shape (B, N)
        scale_w = abs_max / 127.0
        scale_w_safe = np.where(scale_w == 0, 1.0, scale_w)
        
        # W has shape (B, K, N), scale_w_safe has shape (B, N) -> reshape to (B, 1, N)
        scale_w_bc = scale_w_safe[:, np.newaxis, :]
        W_q = np.clip(np.round(W / scale_w_bc), -128, 127).astype(np.int8)
        W_rec = W_q.astype(np.float32) * scale_w_bc
        
        err = W - W_rec
        sq_err = err ** 2
        mse = float(np.mean(sq_err))
        max_err = float(np.max(np.abs(err)))
        
        total_elements_3d += W.size
        total_sq_err_3d += float(np.sum(sq_err))
        if max_err > global_max_err_3d:
            global_max_err_3d = max_err
            
        all_scales_3d.extend(scale_w.flatten().tolist())
        
        cat = name.split(".")[-1]
        by_category_3d.setdefault(cat, []).append({
            "name": name,
            "shape": (B, K, N),
            "mse": mse,
            "max_err": max_err,
            "scales": scale_w.flatten(),
            "numel": W.size
        })
        
    global_mse_3d = total_sq_err_3d / total_elements_3d
    all_scales_3d = np.array(all_scales_3d, dtype=np.float32)
    
    print(f"Total 3D tensors evaluated: {len(gemm_3d_weights)}")
    print(f"Total elements: {total_elements_3d:,} ({total_elements_3d * 2 / 1024 / 1024:.2f} MB in FP16, {total_elements_3d / 1024 / 1024:.2f} MB in INT8)")
    print(f"Global Weighted MSE: {global_mse_3d:.8e} (RMSE: {np.sqrt(global_mse_3d):.6f})")
    print(f"Global Max Absolute Error: {global_max_err_3d:.8f}")
    print(f"Scale Distribution (N={len(all_scales_3d):,} scales):")
    print(f"  Min:    {np.min(all_scales_3d):.8f}")
    print(f"  Median: {np.median(all_scales_3d):.8f}")
    print(f"  Mean:   {np.mean(all_scales_3d):.8f}")
    print(f"  Max:    {np.max(all_scales_3d):.8f}")
    print(f"  Std:    {np.std(all_scales_3d):.8f}")
    
    print("\n--- Breakdown by 3D Tensor Category ---")
    for cat, items in sorted(by_category_3d.items()):
        cat_numel = sum(it["numel"] for it in items)
        cat_sq_err = sum(it["mse"] * it["numel"] for it in items)
        cat_mse = cat_sq_err / cat_numel
        cat_max_err = max(it["max_err"] for it in items)
        cat_scales = np.concatenate([it["scales"] for it in items])
        shapes = sorted(list(set(str(it["shape"]) for it in items)))
        print(f"{cat:<30} count={len(items):<3} shapes={shapes} numel={cat_numel:>10,} MSE={cat_mse:.3e} MaxErr={cat_max_err:.6f} ScaleMean={np.mean(cat_scales):.6f} ScaleMax={np.max(cat_scales):.6f}")

    # Also evaluate ffn_branch_projection_weight and ffn_expand_weight (4D/5D) in their GEMM form
    print("\n=================== FFN BRANCH/EXPAND GEMM PROJECTION RESULTS ===================")
    branch_proj_tensors = {k: v for k, v in tensors.items() if "ffn_branch_projection_weight" in k}
    expand_tensors = {k: v for k, v in tensors.items() if "ffn_expand_weight" in k}
    
    print(f"ffn_branch_projection_weight count: {len(branch_proj_tensors)}")
    print(f"ffn_expand_weight count: {len(expand_tensors)}")
    
    # ffn_branch_projection_weight: (G, 4, 32, 32) -> G slices of (128, 32)
    branch_mses = []
    branch_max_errs = []
    branch_scales = []
    branch_numel = 0
    branch_sq_err = 0.0
    for name, W_f16 in sorted(branch_proj_tensors.items()):
        W = W_f16.astype(np.float32)
        G = W.shape[0]
        # Reshape to (G, 128, 32)
        W_gemm = W.reshape(G, 128, 32)
        abs_max = np.max(np.abs(W_gemm), axis=1) # (G, 32)
        scale_w = abs_max / 127.0
        scale_w_safe = np.where(scale_w == 0, 1.0, scale_w)
        scale_w_bc = scale_w_safe[:, np.newaxis, :]
        W_q = np.clip(np.round(W_gemm / scale_w_bc), -128, 127).astype(np.int8)
        W_rec = W_q.astype(np.float32) * scale_w_bc
        err = W_gemm - W_rec
        sq_err = err ** 2
        branch_numel += W.size
        branch_sq_err += float(np.sum(sq_err))
        branch_mses.append(float(np.mean(sq_err)))
        branch_max_errs.append(float(np.max(np.abs(err))))
        branch_scales.extend(scale_w.flatten().tolist())
    
    branch_scales = np.array(branch_scales, dtype=np.float32)
    print(f"ffn_branch_projection_weight (as G slices of (128, 32)):")
    print(f"  Total elements: {branch_numel:,} ({branch_numel * 2 / 1024 / 1024:.2f} MB in FP16)")
    print(f"  Global MSE:     {branch_sq_err / branch_numel:.8e} (RMSE: {np.sqrt(branch_sq_err / branch_numel):.6f})")
    print(f"  Max Error:      {max(branch_max_errs):.8f}")
    print(f"  Scale Mean:     {np.mean(branch_scales):.8f}, Scale Max: {np.max(branch_scales):.8f}")

if __name__ == "__main__":
    analyze_all_weights()
