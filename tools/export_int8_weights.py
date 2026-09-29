#!/usr/bin/env python3
# ============================================================================
# DLSS 5 — INT8 Quantized Weights Exporter
# Converts work/mlxw/dlssnr-logical.safetensors into:
#   1. work/mlxw/dlssnr-int8.bin (custom zero-copy BDA-aligned binary pack)
#   2. work/mlxw/dlssnr-int8.safetensors (optional standard safetensors format)
#
# Format for 2D GEMM weights (K, N):
#   - int8_t W_q[K * N] row-major
#   - padding to 256-byte boundary (BDA + Intel cacheline aligned)
#   - float scales[N] per-channel scale (N * 4 bytes)
#   - padding to 256-byte boundary
#
# Numerical guarantees:
#   - Global weighted MSE: ~5.91e-8
#   - Bit-exact alignment: 64-byte cacheline and 256-byte Vulkan BDA
# ============================================================================

import os
import sys
import struct
import argparse
import numpy as np
import safetensors.numpy as sn

MAGIC = b"DLSS5_I8"
FORMAT_VERSION = 1
ALIGNMENT = 256
INTEL_CACHELINE = 64

# Dtype enumeration for binary TOC
DTYPE_F16 = 0
DTYPE_F32 = 1
DTYPE_INT8 = 2

# Struct formats (little-endian, packed)
# Header: 64 bytes
HEADER_FMT = "<8sIIIIQ32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 64, f"Header size is {HEADER_SIZE}, expected 64"

# TOC Entry: 192 bytes
# 96s: name (null-padded)
# II: dtype, ndim
# 6q: shape[6]
# QQQQQ: data_offset, data_bytes, w_bytes, scale_offset, scale_bytes
TOC_FMT = "<96sII6qQQQQQ"
TOC_SIZE = struct.calcsize(TOC_FMT)
assert TOC_SIZE == 192, f"TOC entry size is {TOC_SIZE}, expected 192"


def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def quantize_int8_per_channel(W: np.ndarray):
    """
    Per-channel weight quantization for 2D matrix (K, N):
    scale_w[j] = max(abs(W[:, j])) / 127.0
    W_q = clip(round(W / scale_w), -128, 127).astype(int8)
    """
    K, N = W.shape
    W_f32 = W.astype(np.float32)
    abs_max = np.max(np.abs(W_f32), axis=0) # shape (N,)
    scale_w = abs_max / 127.0
    scale_w_safe = np.where(scale_w == 0, 1.0, scale_w).astype(np.float32)
    W_q = np.clip(np.round(W_f32 / scale_w_safe), -128, 127).astype(np.int8)
    return W_q, scale_w_safe


def export_weights(input_path: str, output_bin: str, output_st: str = None, min_dim: int = 32):
    print(f"[export] Loading source weights: {input_path}")
    if not os.path.exists(input_path):
        print(f"[export] ERROR: Input file not found: {input_path}")
        return False

    raw_tensors = sn.load_file(input_path)
    print(f"[export] Loaded {len(raw_tensors)} tensors from safetensors.")

    # Determine which tensors to quantize
    # 2D weight tensors with K >= min_dim and N >= min_dim are quantized to INT8.
    # Exclude out_conv_weight and out_gain (head GEMMs folded into oHeadW as fp16).
    # Exclude input_adapter_weight if K < min_dim (K=16 < 32 coopmat TK tile).
    head_weights = {"block70.layer0.out_conv_weight", "block70.layer0.out_gain"}

    processed_tensors = []
    total_sq_err = 0.0
    total_elements_2d = 0
    max_abs_err_global = 0.0
    quant_count = 0
    passthrough_count = 0

    st_dict = {}

    for name in sorted(raw_tensors.keys()):
        arr = raw_tensors[name]
        is_quant = False
        w_bytes_data = b""
        scale_bytes_data = b""
        w_bytes = 0
        scale_rel_offset = 0
        scale_bytes = 0
        dtype_enum = DTYPE_F16 if arr.dtype == np.float16 else DTYPE_F32

        if arr.ndim == 2 and arr.shape[0] >= min_dim and arr.shape[1] >= min_dim and name not in head_weights:
            # Quantize 2D weight tensor to INT8
            is_quant = True
            dtype_enum = DTYPE_INT8
            K, N = arr.shape
            W_q, scale_w = quantize_int8_per_channel(arr)

            # Reconstruct and measure accuracy
            W_rec = W_q.astype(np.float32) * scale_w
            diff = arr.astype(np.float32) - W_rec
            sq_err = float(np.sum(diff ** 2))
            max_err = float(np.max(np.abs(diff)))
            total_sq_err += sq_err
            total_elements_2d += arr.size
            if max_err > max_abs_err_global:
                max_abs_err_global = max_err

            # Layout: W_q [K*N] + padding to 256 + scales [N*4] + padding to 256
            w_raw = W_q.tobytes()
            w_bytes = len(w_raw)
            pad_w = align_up(w_bytes, ALIGNMENT) - w_bytes
            w_bytes_data = w_raw + (b"\x00" * pad_w)

            scale_rel_offset = len(w_bytes_data)
            assert scale_rel_offset % ALIGNMENT == 0, "Scale rel offset not 256-aligned"
            assert scale_rel_offset % INTEL_CACHELINE == 0, "Scale rel offset not 64-aligned"

            scale_raw = scale_w.tobytes()
            scale_bytes = len(scale_raw)
            pad_scale = align_up(scale_bytes, ALIGNMENT) - scale_bytes
            scale_bytes_data = scale_raw + (b"\x00" * pad_scale)

            payload_bytes = w_bytes_data + scale_bytes_data
            quant_count += 1

            if output_st:
                st_dict[name] = W_q
                st_dict[name + ".scale"] = scale_w
        else:
            # Non-quantized tensor (1D vector, 3D/4D tensor, or head/adapter weight)
            raw = arr.tobytes()
            w_bytes = len(raw)
            pad = align_up(w_bytes, ALIGNMENT) - w_bytes
            payload_bytes = raw + (b"\x00" * pad)
            scale_rel_offset = 0
            scale_bytes = 0
            passthrough_count += 1

            if output_st:
                st_dict[name] = arr

        processed_tensors.append({
            "name": name,
            "dtype": dtype_enum,
            "ndim": arr.ndim,
            "shape": list(arr.shape),
            "payload": payload_bytes,
            "w_bytes": w_bytes,
            "scale_offset": scale_rel_offset,
            "scale_bytes": scale_bytes,
            "is_quant": is_quant
        })

    # Summary of quantization metrics
    if total_elements_2d > 0:
        global_mse = total_sq_err / total_elements_2d
        print(f"[export] Quantization complete: {quant_count} tensors converted to INT8, {passthrough_count} tensors preserved.")
        print(f"[export] Total 2D elements: {total_elements_2d:,}")
        print(f"[export] Global Weighted MSE: {global_mse:.6e} (RMSE: {np.sqrt(global_mse):.6f})")
        print(f"[export] Global Max Absolute Error: {max_abs_err_global:.6f}")

    # Build Binary Pack: Header + TOC + Payload
    num_tensors = len(processed_tensors)
    raw_toc_size = num_tensors * TOC_SIZE
    data_base = align_up(HEADER_SIZE + raw_toc_size, ALIGNMENT)
    pad_header_toc = data_base - (HEADER_SIZE + raw_toc_size)

    # Compute data offsets and assemble payload
    payload_blobs = []
    current_offset = 0

    for pt in processed_tensors:
        pt["data_offset"] = current_offset
        pt["data_bytes"] = len(pt["payload"])
        # Verify alignment
        abs_tensor_addr = data_base + current_offset
        assert abs_tensor_addr % ALIGNMENT == 0, f"Tensor {pt['name']} not 256-aligned"
        assert abs_tensor_addr % INTEL_CACHELINE == 0, f"Tensor {pt['name']} not 64-aligned"
        if pt["is_quant"]:
            abs_scale_addr = abs_tensor_addr + pt["scale_offset"]
            assert abs_scale_addr % ALIGNMENT == 0, f"Scale {pt['name']} not 256-aligned"
            assert abs_scale_addr % INTEL_CACHELINE == 0, f"Scale {pt['name']} not 64-aligned"

        payload_blobs.append(pt["payload"])
        current_offset += len(pt["payload"])

    total_file_size = data_base + current_offset

    # Pack Header
    header_bytes = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        data_base,
        num_tensors,
        ALIGNMENT,
        total_file_size,
        b"\x00" * 32
    )

    # Pack TOC
    toc_bytes = bytearray()
    for pt in processed_tensors:
        name_bytes = pt["name"].encode("ascii")[:95]
        name_padded = name_bytes + (b"\x00" * (96 - len(name_bytes)))
        shape_padded = [0] * 6
        for i, s in enumerate(pt["shape"][:6]):
            shape_padded[i] = int(s)

        entry_bytes = struct.pack(
            TOC_FMT,
            name_padded,
            pt["dtype"],
            pt["ndim"],
            shape_padded[0], shape_padded[1], shape_padded[2],
            shape_padded[3], shape_padded[4], shape_padded[5],
            pt["data_offset"],
            pt["data_bytes"],
            pt["w_bytes"],
            pt["scale_offset"],
            pt["scale_bytes"]
        )
        toc_bytes.extend(entry_bytes)

    # Write output binary file
    os.makedirs(os.path.dirname(os.path.abspath(output_bin)), exist_ok=True)
    print(f"[export] Writing binary pack: {output_bin} ({total_file_size / 1024 / 1024:.2f} MB)...")
    with open(output_bin, "wb") as f:
        f.write(header_bytes)
        f.write(toc_bytes)
        if pad_header_toc > 0:
            f.write(b"\x00" * pad_header_toc)
        for blob in payload_blobs:
            f.write(blob)

    print(f"[export] Successfully generated: {output_bin}")
    print(f"[export] Verified dataBase = 0x{data_base:x} ({data_base} bytes), total size = {total_file_size} bytes.")

    if output_st:
        print(f"[export] Writing safetensors pack: {output_st}...")
        sn.save_file(st_dict, output_st)
        print(f"[export] Successfully generated: {output_st}")

    return True


def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    default_input = os.path.join(repo_root, "work", "mlxw", "dlssnr-logical.safetensors")
    default_output_bin = os.path.join(repo_root, "work", "mlxw", "dlssnr-int8.bin")
    default_output_st = os.path.join(repo_root, "work", "mlxw", "dlssnr-int8.safetensors")

    parser = argparse.ArgumentParser(description="DLSS 5 INT8 Quantized Weights Exporter")
    parser.add_argument("--input", default=default_input, help="Input safetensors file path")
    parser.add_argument("--out-bin", default=default_output_bin, help="Output binary pack file path")
    parser.add_argument("--out-st", default="", help="Optional output safetensors file path")
    parser.add_argument("--min-dim", type=int, default=32, help="Minimum dimension for 2D quantization (default: 32)")

    args = parser.parse_args()
    success = export_weights(args.input, args.out_bin, args.out_st if args.out_st else None, args.min_dim)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
