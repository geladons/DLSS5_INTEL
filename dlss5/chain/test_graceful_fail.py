#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# DLSS 5 regression test: graceful init failure inside the HOST process.
# d5c_chain.dll runs inside game processes (m11-layer x64, m12-dxgi) - a
# missing weights/shader file must NEVER std::exit() the host. Before the
# fix, dlss5_process_frame -> ensure() -> engine.init -> ws_.load() called
# std::exit(1) on a missing safetensors and this script simply vanished
# (no FAIL line, silent exit code 1 - exactly what Witcher 3 / Metro / RDR2
# hit on their first processed present).
# After the fix the script MUST reach the end: process_frame returns nonzero,
# the failure latches for that resolution, and a fresh context with valid
# weights+shaders still initializes and processes (the latch is per-context
# and releases on config change - it is not a global kill switch).
# Usage: python test_graceful_fail.py [path-to-d5c_chain.dll]
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

    # Deterministic BGRA input, alpha 255.
    w, h = 320, 180
    rng = np.random.RandomState(0)
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[:, :, :3] = rng.randint(0, 256, (h, w, 3))
    img[:, :, 3] = 255
    in_bytes = img.tobytes()
    out = np.zeros_like(img)
    out_ptr = out.ctypes.data_as(ctypes.c_void_p)

    missing_weights = b"C:\\definitely\\missing.safetensors"
    shader_dir_bytes = (str(shader_dir) + "\\").encode('utf-8')

    all_ok = True

    # ---- Phase 1: bad weights path must fail WITHOUT killing this process --
    print("[TEST] Phase 1: dlss5_create with a missing weights file")
    bad = chain.dlss5_create(missing_weights, shader_dir_bytes, 1.0, 1.0, 1.0)
    if not bad:
        print("[FAIL] dlss5_create returned NULL for missing weights "
              "(expected a context - init is lazy)")
        return 1   # process survived to print this either way
    print("[TEST] context created; calling dlss5_process_frame "
          "(pre-fix the DLL exit(1)'d HERE - this line never printed)")

    r1 = chain.dlss5_process_frame(bad, w, h, in_bytes, out_ptr, ctypes.c_long(1))
    print(f"[TEST] process_frame #1 returned {r1}; host process still alive")
    if r1 == 0:
        print("[FAIL] process_frame succeeded with missing weights?!")
        all_ok = False

    # Same resolution again: the init_failed latch must make this a fast -1
    # (no vkCreateInstance/weights retry per frame).
    r2 = chain.dlss5_process_frame(bad, w, h, in_bytes, out_ptr, ctypes.c_long(2))
    print(f"[TEST] process_frame #2 (same dims, latched) returned {r2}")
    if r2 == 0:
        print("[FAIL] latched context unexpectedly succeeded")
        all_ok = False

    # Different resolution: latch releases, init retried, fails again cleanly.
    w2, h2 = 352, 200
    img2 = np.empty((h2, w2, 4), dtype=np.uint8)
    img2[:, :, :3] = rng.randint(0, 256, (h2, w2, 3))
    img2[:, :, 3] = 255
    out2 = np.zeros_like(img2)
    r3 = chain.dlss5_process_frame(bad, w2, h2, img2.tobytes(),
                                   out2.ctypes.data_as(ctypes.c_void_p),
                                   ctypes.c_long(3))
    print(f"[TEST] process_frame #3 (new dims, retry) returned {r3}")
    if r3 == 0:
        print("[FAIL] retry at new dims unexpectedly succeeded")
        all_ok = False

    chain.dlss5_destroy(bad)
    print("[TEST] bad context destroyed; host process still alive")

    # ---- Phase 2: a NEW context with valid weights+shaders must work -------
    print("[TEST] Phase 2: fresh context with valid weights+shaders")
    good = chain.dlss5_create(str(weights_path).encode('utf-8'),
                              shader_dir_bytes, 1.0, 1.0, 1.0)
    if not good:
        print("[FAIL] dlss5_create returned NULL for valid config")
        return 1
    r4 = chain.dlss5_process_frame(good, w, h, in_bytes, out_ptr, ctypes.c_long(1))
    print(f"[TEST] process_frame on valid context returned {r4}")
    if r4 != 0:
        print("[FAIL] valid context could not process (latch leaked?)")
        all_ok = False
    else:
        same_io = np.array_equal(out, img)
        print(f"[SANITY] output == input? {same_io} (chain actually ran: {not same_io})")
        if same_io:
            print("[FAIL] output byte-identical to input - chain did not run")
            all_ok = False
    chain.dlss5_destroy(good)
    print("[TEST] good context destroyed")

    if all_ok:
        print("GRACEFUL FAIL TEST PASS")
        return 0
    print("GRACEFUL FAIL TEST FAIL")
    return 1

if __name__ == "__main__":
    sys.exit(main())
