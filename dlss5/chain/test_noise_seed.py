#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# DLSS 5 regression test: pinned vendor noise seed.
# Processes the SAME input frame with different frame_index values and checks
# that outputs are bit-identical (per-frame seed used to shimmer static
# content; the seed is now pinned to a constant).
# A build with the pre-fix frameIndex-keyed seed (e.g. commit 196c8df..b584724) FAILS this test.
# Usage: python test_noise_seed.py [path-to-d5c_chain.dll]
# Exit: 0 = PASS, 1 = FAIL, 2 = SKIP (weights or DLL missing).
# ============================================================================
import os
import sys
import ctypes
from pathlib import Path
import numpy as np

def main():
    repo_root = Path(__file__).resolve().parents[2]

    weights_path = repo_root / "work" / "mlxw" / "dlssnr-logical.safetensors"
    if not weights_path.exists():
        print(f"[SKIP] weights not found: {weights_path}")
        return 2

    if len(sys.argv) > 1:
        dll_path = Path(sys.argv[1])
        if not dll_path.exists():
            print(f"[SKIP] d5c_chain.dll not found: {dll_path}")
            return 2
    else:
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
            print("[SKIP] d5c_chain.dll not found in candidate paths")
            return 2

    shader_dir = repo_root / "dlss5" / "m11d" / "build" / "Release"
    if not shader_dir.exists():
        print(f"[SKIP] shader dir not found: {shader_dir}")
        return 2

    print(f"[TEST] Loading DLSS 5 Chain DLL: {dll_path}")
    chain = ctypes.CDLL(str(dll_path))

    chain.dlss5_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_float]
    chain.dlss5_create.restype = ctypes.c_void_p

    chain.dlss5_destroy.argtypes = [ctypes.c_void_p]
    chain.dlss5_destroy.restype = None

    chain.dlss5_process_frame.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long
    ]
    chain.dlss5_process_frame.restype = ctypes.c_int

    chain.dlss5_set_scale.argtypes = [ctypes.c_void_p, ctypes.c_float]
    chain.dlss5_set_scale.restype = None

    weights_bytes = str(weights_path).encode('utf-8')
    shader_dir_bytes = (str(shader_dir) + "\\").encode('utf-8')

    # Deterministic textured BGRA input: noise + gradient, alpha 255.
    w, h = 640, 384
    rng = np.random.RandomState(0)
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[:, :, 0] = (rng.randint(0, 256, (h, w)) * 0.6 + (xx / w) * 100).astype(np.uint8)
    img[:, :, 1] = (rng.randint(0, 256, (h, w)) * 0.6 + (yy / h) * 100).astype(np.uint8)
    img[:, :, 2] = rng.randint(0, 256, (h, w)).astype(np.uint8)
    img[:, :, 3] = 255
    in_bytes = img.tobytes()

    print("[TEST] Initializing DLSS 5 Engine Context...")
    handle = chain.dlss5_create(weights_bytes, shader_dir_bytes, 1.0, 1.0, 1.0)
    if not handle:
        print("[FAIL] dlss5_create returned NULL")
        return 1

    all_ok = True
    try:
        for scale in (1.0, 0.5):
            chain.dlss5_set_scale(handle, scale)
            ref = np.zeros_like(img)
            ret = chain.dlss5_process_frame(handle, w, h, in_bytes,
                                            ref.ctypes.data_as(ctypes.c_void_p),
                                            ctypes.c_long(1))
            if ret != 0:
                print(f"[FAIL] scale={scale}: dlss5_process_frame(frame_index=1) returned {ret}")
                all_ok = False
                continue

            ref_bytes = ref.tobytes()
            diff_ref = np.frombuffer(ref_bytes, dtype=np.uint8).reshape(img.shape)
            same_io = np.array_equal(diff_ref, img)
            print(f"[SANITY] scale={scale}: output == input? {same_io} (chain actually ran: {not same_io})")

            for fi in (2, 12345):
                out = np.zeros_like(img)
                ret = chain.dlss5_process_frame(handle, w, h, in_bytes,
                                                out.ctypes.data_as(ctypes.c_void_p),
                                                ctypes.c_long(fi))
                if ret != 0:
                    print(f"[FAIL] scale={scale}: dlss5_process_frame(frame_index={fi}) returned {ret}")
                    all_ok = False
                    continue
                out_bytes = out.tobytes()
                a = np.frombuffer(ref_bytes, dtype=np.uint8).astype(np.int32)
                b = np.frombuffer(out_bytes, dtype=np.uint8).astype(np.int32)
                d = np.abs(a - b)
                max_diff = int(d.max())
                n_diff = int(np.count_nonzero(d))
                identical = (n_diff == 0)
                print(f"[TEST] scale={scale} frame_index={fi} vs 1: max abs diff={max_diff}, differing bytes={n_diff}, identical={identical}")
                if not identical:
                    all_ok = False
    finally:
        chain.dlss5_destroy(handle)
        print("[TEST] Engine context destroyed.")

    if all_ok:
        print("NOISE SEED TEST PASS")
        return 0
    print("NOISE SEED TEST FAIL")
    return 1

if __name__ == "__main__":
    sys.exit(main())
