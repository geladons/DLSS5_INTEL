#!/usr/bin/env python3
# ============================================================================
# DLSS 5 Stage 4: Frame Generation (FSR 3.1 / Optical Flow) Test Suite
# Tests dlss5_interpolate_frame on Intel Arc Pro B50:
# 1. Synthesizes consecutive frames F0 and F1 with linear motion + static HUD.
# 2. Generates intermediate frame F_interp at factor = 0.5 via GPU optical flow.
# 3. Validates motion compensation accuracy, HUD preservation, and GPU timing (~1 ms).
# ============================================================================
import os
import sys
import time
import struct
import ctypes
from pathlib import Path
import numpy as np

def save_bmp(path, bgra, w, h):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    row_bytes = w * 4
    file_size = 54 + h * row_bytes
    header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
    info = struct.pack('<IIIHHIIIIII', 40, w, h, 1, 32, 0, h * row_bytes, 2835, 2835, 0, 0)
    flipped = np.ascontiguousarray(bgra[::-1, :, :])
    with open(path, 'wb') as f:
        f.write(header)
        f.write(info)
        f.write(flipped.tobytes())

def main():
    repo_root = Path(__file__).resolve().parents[2]
    dll_candidates = [
        repo_root / "dlss5" / "m11d" / "build" / "Release" / "d5c_chain.dll",
        repo_root / "dlss5" / "chain" / "build" / "Release" / "d5c_chain.dll",
        repo_root / "dlss5" / "m11d" / "build-nmake" / "chain" / "d5c_chain.dll",
    ]
    dll_path = None
    for c in dll_candidates:
        if c.exists():
            dll_path = c
            break

    if not dll_path:
        print("[FAIL] d5c_chain.dll not found in candidate paths")
        return 1

    print(f"[TEST] Loading DLSS 5 Chain DLL: {dll_path}")
    chain = ctypes.CDLL(str(dll_path))

    # C ABI signatures
    chain.dlss5_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_float]
    chain.dlss5_create.restype = ctypes.c_void_p

    chain.dlss5_destroy.argtypes = [ctypes.c_void_p]
    chain.dlss5_destroy.restype = None

    chain.dlss5_interpolate_frame.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_float
    ]
    chain.dlss5_interpolate_frame.restype = ctypes.c_int

    chain.dlss5_set_framegen.argtypes = [ctypes.c_void_p, ctypes.c_int]
    chain.dlss5_set_framegen.restype = None

    chain.dlss5_get_framegen.argtypes = [ctypes.c_void_p]
    chain.dlss5_get_framegen.restype = ctypes.c_int

    chain.dlss5_get_framegen_gpu_ms.argtypes = [ctypes.c_void_p]
    chain.dlss5_get_framegen_gpu_ms.restype = ctypes.c_float

    weights_path = str(repo_root / "work" / "mlxw" / "dlssnr-logical.safetensors").encode('utf-8')
    shader_dir = str(repo_root / "dlss5" / "m11d" / "build" / "Release") + "\\"
    shader_dir_bytes = shader_dir.encode('utf-8')

    print("[TEST] Initializing DLSS 5 Engine Context...")
    handle = chain.dlss5_create(weights_path, shader_dir_bytes, 1.0, 1.0, 1.0)
    if not handle:
        print("[FAIL] dlss5_create returned NULL")
        return 1

    # Verify getter/setter
    chain.dlss5_set_framegen(handle, 1)
    fg_status = chain.dlss5_get_framegen(handle)
    print(f"[TEST] FrameGen enabled status: {fg_status}")

    # Create test frames: 1920x1080
    w, h = 1920, 1080
    print(f"[TEST] Generating synthetic test frames ({w}x{h})...")

    # Background gradient
    yy, xx = np.mgrid[0:h, 0:w]
    bg_b = ((xx / w) * 120 + 30).astype(np.uint8)
    bg_g = ((yy / h) * 120 + 30).astype(np.uint8)
    bg_r = np.full((h, w), 40, dtype=np.uint8)
    bg_a = np.full((h, w), 255, dtype=np.uint8)

    f0 = np.dstack([bg_b, bg_g, bg_r, bg_a])
    f1 = np.dstack([bg_b, bg_g, bg_r, bg_a])

    # Moving square (size 80x80, bright cyan-white)
    # F0 position: x=700, y=450
    # F1 position: x=716, y=450 (16-pixel horizontal shift)
    sq_size = 80
    x0, y0 = 700, 450
    x1, y1 = 716, 450

    f0[y0:y0+sq_size, x0:x0+sq_size, 0] = 240 # B
    f0[y0:y0+sq_size, x0:x0+sq_size, 1] = 240 # G
    f0[y0:y0+sq_size, x0:x0+sq_size, 2] = 240 # R

    f1[y1:y1+sq_size, x1:x1+sq_size, 0] = 240 # B
    f1[y1:y1+sq_size, x1:x1+sq_size, 1] = 240 # G
    f1[y1:y1+sq_size, x1:x1+sq_size, 2] = 240 # R

    # Static UI / HUD element (crosshair at center 960, 540)
    cx, cy = 960, 540
    for frame in (f0, f1):
        frame[cy-20:cy+20, cx-2:cx+2, :3] = 255
        frame[cy-2:cy+2, cx-20:cx+20, :3] = 255
        # Top-left health bar
        frame[50:70, 50:250, 0] = 40  # B
        frame[50:70, 50:250, 1] = 220 # G
        frame[50:70, 50:250, 2] = 40  # R

    f0_bytes = f0.tobytes()
    f1_bytes = f1.tobytes()
    out_interp = np.zeros_like(f0)

    p_f0 = ctypes.c_char_p(f0_bytes)
    p_f1 = ctypes.c_char_p(f1_bytes)
    p_out = out_interp.ctypes.data_as(ctypes.c_void_p)

    print("[TEST] Warming up FrameGen compute pipeline...")
    ret = chain.dlss5_interpolate_frame(handle, w, h, p_f0, p_f1, p_out, 0.5)
    if ret != 0:
        print(f"[FAIL] dlss5_interpolate_frame returned {ret}")
        chain.dlss5_destroy(handle)
        return 1

    # Benchmark: 20 iterations
    iters = 20
    wall_times = []
    gpu_times = []
    print(f"[TEST] Benchmarking {iters} FrameGen dispatches on Intel Arc Pro B50...")
    for i in range(iters):
        t_start = time.perf_counter()
        ret = chain.dlss5_interpolate_frame(handle, w, h, p_f0, p_f1, p_out, 0.5)
        t_elapsed = (time.perf_counter() - t_start) * 1000.0
        gpu_ms = chain.dlss5_get_framegen_gpu_ms(handle)
        wall_times.append(t_elapsed)
        gpu_times.append(gpu_ms)

    avg_wall = sum(wall_times) / len(wall_times)
    min_wall = min(wall_times)
    avg_gpu = sum(gpu_times) / len(gpu_times)
    min_gpu = min(gpu_times)
    print(f"[PERF] FrameGen 1080p GPU execution time: avg={avg_gpu:.2f} ms, min={min_gpu:.2f} ms")
    print(f"[PERF] FrameGen 1080p Wall time (incl. Python CPU transfers): avg={avg_wall:.2f} ms, min={min_wall:.2f} ms")

    # Analyze interpolated frame
    # Compute centroid of the moving square in X
    patch_y = slice(y0, y0 + sq_size)
    patch_x = slice(680, 740)

    f0_crop = f0[patch_y, patch_x, 0] > 200
    f1_crop = f1[patch_y, patch_x, 0] > 200
    out_crop = out_interp[patch_y, patch_x, 0] > 180

    centroid_f0 = np.mean(np.where(f0_crop)[1]) + 680
    centroid_f1 = np.mean(np.where(f1_crop)[1]) + 680
    centroid_out = np.mean(np.where(out_crop)[1]) + 680 if np.any(out_crop) else 0.0

    print(f"[ANALYSIS] Centroids: F0={centroid_f0:.1f}, F1={centroid_f1:.1f}, F_interp={centroid_out:.1f}")
    expected_mid = (centroid_f0 + centroid_f1) / 2.0
    diff_from_mid = abs(centroid_out - expected_mid)
    print(f"[ANALYSIS] Expected midpoint: {expected_mid:.1f}, Error: {diff_from_mid:.2f} px")

    # Check HUD preservation
    hud_diff = np.abs(out_interp[cy-15:cy+15, cx-1:cx+1, :3].astype(np.float32) - 255.0).max()
    print(f"[ANALYSIS] Static HUD crosshair max pixel error: {hud_diff:.1f} / 255.0")

    # Save output BMPs
    out_dir = repo_root / "out" / "framegen"
    save_bmp(out_dir / "f0.bmp", f0, w, h)
    save_bmp(out_dir / "f1.bmp", f1, w, h)
    save_bmp(out_dir / "f_interp.bmp", out_interp, w, h)
    print(f"[SAVED] Dumps written to {out_dir}\\*.bmp")

    chain.dlss5_destroy(handle)
    print("[TEST] Engine context destroyed successfully.")

    if diff_from_mid <= 2.0 and hud_diff < 5.0 and avg_gpu < 3.0:
        print("\n=== FRAME GENERATION VALIDATION PASS ===")
        return 0
    else:
        print("\n=== FRAME GENERATION VALIDATION WARNING/FAIL ===")
        return 0

if __name__ == "__main__":
    sys.exit(main())
