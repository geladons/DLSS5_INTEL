#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# DLSS 5 Stage 4: Frame Generation residual-transfer test (1:2 cadence).
# Tests dlss5_framegen_transfer on Intel Arc Pro B50:
#  T1 before any process_frame the call fails cleanly (non-zero, no crash).
#  T2 static scene: transfer(R) after process(R) reproduces the processed
#     frame within +-1 LSB (max) and < 0.01 mean over all channels.
#  T3 motion: R1 = roll(R, 6 px); transfer(R1) must sit measurably closer to
#     roll(process(R), 6) than the naive native frame does.
#  T4 GPU time of the transfer dispatch at the test resolution (< 10 ms).
# ============================================================================
import os
import sys
import ctypes
from pathlib import Path
import numpy as np

W, H = 640, 384


def make_raw(w, h):
    """Deterministic raw frame: gradients + shapes + noise; 32px black border
    keeps np.roll wrap-around harmless."""
    rng = np.random.RandomState(20260930)
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), dtype=np.uint8)
    img[:, :, 0] = ((xx / w) * 120 + 30).astype(np.uint8)          # B gradient
    img[:, :, 1] = ((yy / h) * 120 + 30).astype(np.uint8)          # G gradient
    img[:, :, 2] = np.full((h, w), 60, dtype=np.uint8)             # R flat
    img[:, :, 3] = 255
    # noise gives the residual something to carry
    noise = rng.randint(0, 28, (h, w), dtype=np.uint8)
    img[:, :, 0] = np.clip(img[:, :, 0].astype(int) + noise, 0, 255)
    img[:, :, 2] = np.clip(img[:, :, 2].astype(int) + noise, 0, 255)
    # shapes with strong edges for the block matcher
    img[100:180, 200:280, :3] = (240, 240, 240)                    # bright square
    img[60:100, 400:560, :3] = (40, 220, 40)                       # green bar
    img[250:300, 100:200, :3] = (30, 30, 230)                      # red block
    img[:32, :, :] = 0
    img[-32:, :, :] = 0
    img[:, :32, :] = 0
    img[:, -32:, :] = 0
    img[:, :, 3] = 255
    return img


def mean_abs(a, b):
    return float(np.abs(a.astype(np.float32) - b.astype(np.float32)).mean())


def main():
    repo_root = Path(__file__).resolve().parents[2]
    dll_candidates = [
        repo_root / "dlss5" / "m11d" / "build" / "Release" / "d5c_chain.dll",
        repo_root / "dlss5" / "chain" / "build" / "Release" / "d5c_chain.dll",
        repo_root / "dlss5" / "m11d" / "build-nmake" / "chain" / "d5c_chain.dll",
    ]
    dll_path = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if not dll_path or not dll_path.exists():
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

    chain.dlss5_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                   ctypes.c_float, ctypes.c_float, ctypes.c_float]
    chain.dlss5_create.restype = ctypes.c_void_p
    chain.dlss5_destroy.argtypes = [ctypes.c_void_p]
    chain.dlss5_destroy.restype = None
    chain.dlss5_process_frame.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_void_p,
                                          ctypes.c_void_p, ctypes.c_long]
    chain.dlss5_process_frame.restype = ctypes.c_int
    chain.dlss5_framegen_transfer.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                              ctypes.c_uint32, ctypes.c_void_p,
                                              ctypes.c_void_p]
    chain.dlss5_framegen_transfer.restype = ctypes.c_int
    chain.dlss5_get_framegen_gpu_ms.argtypes = [ctypes.c_void_p]
    chain.dlss5_get_framegen_gpu_ms.restype = ctypes.c_float

    weights_path = str(repo_root / "work" / "mlxw" / "dlssnr-logical.safetensors").encode("utf-8")
    shader_dir = (str(repo_root / "dlss5" / "m11d" / "build" / "Release") + "\\").encode("utf-8")

    print("[TEST] Initializing DLSS 5 Engine Context...")
    handle = chain.dlss5_create(weights_path, shader_dir, 1.0, 1.0, 1.0)
    if not handle:
        print("[FAIL] dlss5_create returned NULL")
        return 1

    raw0 = make_raw(W, H)
    out_p = np.zeros_like(raw0)
    out_g = np.zeros_like(raw0)
    p_out_p = out_p.ctypes.data_as(ctypes.c_void_p)
    p_out_g = out_g.ctypes.data_as(ctypes.c_void_p)

    fails = []

    # T1: transfer before any process_frame must fail cleanly
    ret = chain.dlss5_framegen_transfer(handle, W, H,
                                        raw0.ctypes.data_as(ctypes.c_void_p), p_out_g)
    print(f"[T1] transfer before first process -> {ret} (expect non-zero)")
    if ret == 0:
        fails.append("T1: transfer succeeded before any processed frame")

    # T2: static - transfer(R) after process(R) == processed frame
    ret = chain.dlss5_process_frame(handle, W, H,
                                    raw0.ctypes.data_as(ctypes.c_void_p), p_out_p, 1)
    if ret != 0:
        print(f"[FAIL] dlss5_process_frame returned {ret}")
        chain.dlss5_destroy(handle)
        return 1
    ret = chain.dlss5_framegen_transfer(handle, W, H,
                                        raw0.ctypes.data_as(ctypes.c_void_p), p_out_g)
    if ret != 0:
        fails.append(f"T2: transfer returned {ret}")
    else:
        d = np.abs(out_g.astype(np.int32) - out_p.astype(np.int32))
        print(f"[T2] static: max|G-P|={d.max()} mean|G-P|={d.mean():.5f}")
        if d.max() > 1 or d.mean() >= 0.01:
            fails.append(f"T2: static mismatch max={d.max()} mean={d.mean():.5f}")

    # T3: motion - transfer(roll(R,6)) vs roll(process(R),6)
    raw1 = np.roll(raw0, 6, axis=1)
    out_p0 = np.zeros_like(raw0)
    ret = chain.dlss5_process_frame(handle, W, H,
                                    raw0.ctypes.data_as(ctypes.c_void_p),
                                    out_p0.ctypes.data_as(ctypes.c_void_p), 2)
    if ret != 0:
        fails.append(f"T3: process_frame returned {ret}")
    ret = chain.dlss5_framegen_transfer(handle, W, H,
                                        raw1.ctypes.data_as(ctypes.c_void_p), p_out_g)
    if ret != 0:
        fails.append(f"T3: transfer returned {ret}")
    else:
        ref = np.roll(out_p0, 6, axis=1)
        s = np.s_[16:-16, 16:-16, :]           # interior (16px margin)
        m_g_ref = mean_abs(out_g[s], ref[s])
        m_r_ref = mean_abs(raw1[s], ref[s])
        m_g_r = mean_abs(out_g[s], raw1[s])
        print(f"[T3] motion: mean|G-Ref|={m_g_ref:.3f} "
              f"mean|R-Ref|={m_r_ref:.3f} mean|G-R|={m_g_r:.3f}")
        if not (m_g_ref < 0.6 * m_r_ref):
            fails.append(f"T3: transfer did not recover detail "
                         f"({m_g_ref:.3f} vs 0.6*{m_r_ref:.3f})")
        if not (m_g_r > 0.0):
            fails.append("T3: generated frame equals raw input")

    # T4: GPU time of the last transfer dispatch
    gpu_ms = chain.dlss5_get_framegen_gpu_ms(handle)
    print(f"[T4] framegen GPU time: {gpu_ms:.3f} ms at {W}x{H} (expect < 10)")
    if not (0.0 < gpu_ms < 10.0):
        fails.append(f"T4: gpu_ms={gpu_ms:.3f}")

    chain.dlss5_destroy(handle)

    if fails:
        for f in fails:
            print(f"[FAIL] {f}")
        print("FRAMEGEN TRANSFER TEST FAIL")
        return 1
    print("FRAMEGEN TRANSFER TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
