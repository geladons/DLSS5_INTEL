#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# DLSS 5 Frame Generation mode 2: anchor-to-anchor midpoint interpolation
# (Lossless-Scaling-class). Tests dlss5_framegen_interpolate on Intel Arc:
#  T1 before any process_frame the call fails cleanly (non-zero, no crash).
#  T2 one-anchor warmup: interpolate copies the newest processed anchor.
#  T3 static scene: midpoint of identical anchors ~= the processed frame.
#  T4 12px roll motion: midpoint recovers the TRUE middle frame (vs the
#     naive crossfade - the extrapolation failure the field reported).
#  T5 scene cut: unrelated anchors -> output ~= midRaw (the real middle
#     frame, temporally honest) and NOT a dissolved ghost.
#  T6 GPU time of the interpolate dispatch at the test resolution (< 10 ms).
# ============================================================================
import os
import sys
import ctypes
from pathlib import Path
import numpy as np

# The gain anchor rescales the processed frames' mean residual toward an
# EMA; generated (warped) frames carry no such correction, so P-vs-G on a
# static scene differs by the anchor's DC (~1.7 luma mean - the feature,
# not a warp bug). This test measures interpolation mechanics only.
os.environ.setdefault("DLSS5_GAIN_LOCK", "0")

# Temporal history is likewise orthogonal to the warp mechanics under
# test, and its hold/release latch (e35791f) actively corrupts T4's
# comparison: the latch snaps both processed anchors toward shared
# history, which flatters the naive crossfade while the generated frame
# (correctly tracking the real midRaw) is measured against a blended
# reference. With temporal on the pass margin collapses from ~9x better
# than crossfade to a coin-flip vs the 0.8 bar - pure baseline noise.
# Temporal behaviour has its own coverage; pin it off here.
os.environ.setdefault("DLSS5_TEMPORAL", "0")

W, H = 640, 384


def make_raw(w, h, seed=20260930):
    rng = np.random.RandomState(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), dtype=np.uint8)
    img[:, :, 0] = ((xx / w) * 120 + 30).astype(np.uint8)
    img[:, :, 1] = ((yy / h) * 120 + 30).astype(np.uint8)
    img[:, :, 2] = np.full((h, w), 60, dtype=np.uint8)
    img[:, :, 3] = 255
    noise = rng.randint(0, 28, (h, w), dtype=np.uint8)
    img[:, :, 0] = np.clip(img[:, :, 0].astype(int) + noise, 0, 255)
    img[:, :, 2] = np.clip(img[:, :, 2].astype(int) + noise, 0, 255)
    img[100:180, 200:280, :3] = (240, 240, 240)
    img[60:100, 400:560, :3] = (40, 220, 40)
    img[250:300, 100:200, :3] = (30, 30, 230)
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
    chain.dlss5_framegen_interpolate.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                 ctypes.c_uint32, ctypes.c_void_p,
                                                 ctypes.c_void_p, ctypes.c_float]
    chain.dlss5_framegen_interpolate.restype = ctypes.c_int
    chain.dlss5_set_framegen_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
    chain.dlss5_set_framegen_mode.restype = None
    chain.dlss5_get_framegen_mode.argtypes = [ctypes.c_void_p]
    chain.dlss5_get_framegen_mode.restype = ctypes.c_int
    chain.dlss5_get_framegen_gpu_ms.argtypes = [ctypes.c_void_p]
    chain.dlss5_get_framegen_gpu_ms.restype = ctypes.c_float

    weights_path = str(repo_root / "work" / "mlxw" / "dlssnr-logical.safetensors").encode("utf-8")
    shader_dir = (str(repo_root / "dlss5" / "m11d" / "build" / "Release") + "\\").encode("utf-8")

    print("[TEST] Initializing DLSS 5 Engine Context...")
    handle = chain.dlss5_create(weights_path, shader_dir, 1.0, 1.0, 1.0)
    if not handle:
        print("[FAIL] dlss5_create returned NULL")
        return 1

    chain.dlss5_set_framegen_mode(handle, 2)
    print(f"[TEST] framegen mode -> {chain.dlss5_get_framegen_mode(handle)} (expect 2)")

    raw0 = make_raw(W, H)
    mid = np.roll(raw0, 6, axis=1)
    raw1 = np.roll(raw0, 12, axis=1)
    out_p = np.zeros_like(raw0)
    out_g = np.zeros_like(raw0)
    vp = lambda a: a.ctypes.data_as(ctypes.c_void_p)

    fails = []

    # T1: interpolate before any process_frame must fail cleanly
    ret = chain.dlss5_framegen_interpolate(handle, W, H, vp(mid), vp(out_g), 0.5)
    print(f"[T1] interpolate before first process -> {ret} (expect non-zero)")
    if ret == 0:
        fails.append("T1: interpolate succeeded before any processed frame")

    # T2: one anchor -> warmup copies the newest processed anchor
    ret = chain.dlss5_process_frame(handle, W, H, vp(raw0), vp(out_p), 1)
    if ret != 0:
        print(f"[FAIL] dlss5_process_frame returned {ret}")
        chain.dlss5_destroy(handle)
        return 1
    ret = chain.dlss5_framegen_interpolate(handle, W, H, vp(mid), vp(out_g), 0.5)
    if ret != 0:
        fails.append(f"T2: warmup interpolate returned {ret}")
    else:
        d = np.abs(out_g.astype(np.int32) - out_p.astype(np.int32))
        print(f"[T2] warmup: max|G-P0|={d.max()} mean={d.mean():.5f} (expect exact)")
        if d.max() > 1:
            fails.append(f"T2: warmup copy mismatch max={d.max()}")

    # T3: static - two identical anchors -> midpoint == processed frame
    ret = chain.dlss5_process_frame(handle, W, H, vp(raw0), vp(out_p), 2)
    if ret != 0:
        fails.append(f"T3: process_frame returned {ret}")
    ret = chain.dlss5_framegen_interpolate(handle, W, H, vp(raw0), vp(out_g), 0.5)
    if ret != 0:
        fails.append(f"T3: interpolate returned {ret}")
    else:
        d = np.abs(out_g.astype(np.int32) - out_p.astype(np.int32))
        # Anchors differ by a hair: the second process ran with temporal
        # history, the AABB history clamp shifts edge pixels a few levels,
        # and bilinear resampling blurs sub-LSB - a small drift is physics,
        # not ghosting.
        print(f"[T3] static: max|G-P|={d.max()} mean={d.mean():.5f}")
        if d.max() > 16 or d.mean() >= 0.5:
            fails.append(f"T3: static midpoint mismatch max={d.max()} mean={d.mean():.5f}")

    # T4: motion - anchors F0, roll(F0,12); truth = processed roll(F0,6).
    # The warped midpoint must beat the naive processed crossfade - this is
    # the interpolation-vs-extrapolation win.
    out_p0 = np.zeros_like(raw0)
    out_p1 = np.zeros_like(raw0)
    chain.dlss5_process_frame(handle, W, H, vp(raw0), vp(out_p0), 3)
    chain.dlss5_process_frame(handle, W, H, vp(raw1), vp(out_p1), 4)
    ret = chain.dlss5_framegen_interpolate(handle, W, H, vp(mid), vp(out_g), 0.5)
    if ret != 0:
        fails.append(f"T4: interpolate returned {ret}")
    else:
        out_pref = np.zeros_like(raw0)
        chain.dlss5_process_frame(handle, W, H, vp(mid), vp(out_pref), 5)
        s = np.s_[16:-16, 16:-16, :]
        naive = ((out_p0.astype(np.float32) + out_p1.astype(np.float32)) * 0.5)
        m_g = mean_abs(out_g[s], out_pref[s])
        m_n = mean_abs(naive[s], out_pref[s])
        print(f"[T4] motion: mean|G-Pref|={m_g:.3f} mean|naive-Pref|={m_n:.3f}")
        if not (m_g < 0.8 * m_n):
            fails.append(f"T4: midpoint no better than crossfade ({m_g:.3f} vs {m_n:.3f})")

    # T5: scene cut - second anchor is unrelated content; the matcher fails
    # and the output must fall back to the real middle frame, not ghost.
    # Pure noise: no shared structure, so no match can pass accidentally.
    rngC = np.random.RandomState(777)
    rawC = (rngC.randint(0, 256, (H, W, 4))).astype(np.uint8)
    rawC[:, :, 3] = 255
    rngM = np.random.RandomState(31337)
    midC = (rngM.randint(0, 256, (H, W, 4))).astype(np.uint8)
    midC[:, :, 3] = 255
    out_pc = np.zeros_like(raw0)
    chain.dlss5_process_frame(handle, W, H, vp(rawC), vp(out_pc), 6)
    ret = chain.dlss5_framegen_interpolate(handle, W, H, vp(midC), vp(out_g), 0.5)
    if ret != 0:
        fails.append(f"T5: interpolate returned {ret}")
    else:
        m_g_mid = mean_abs(out_g, midC)
        naive = ((out_pref.astype(np.float32) + out_pc.astype(np.float32)) * 0.5)
        m_n_mid = mean_abs(naive.astype(np.uint8), midC)
        print(f"[T5] cut: mean|G-mid|={m_g_mid:.3f} mean|naive-mid|={m_n_mid:.3f}")
        if not (m_g_mid < 0.5 * m_n_mid):
            fails.append(f"T5: cut output not midRaw-anchored ({m_g_mid:.3f} vs {m_n_mid:.3f})")

    # T6: GPU time of the last interpolate dispatch
    gpu_ms = chain.dlss5_get_framegen_gpu_ms(handle)
    print(f"[T6] framegen GPU time: {gpu_ms:.3f} ms at {W}x{H} (expect < 10)")
    if not (0.0 < gpu_ms < 10.0):
        fails.append(f"T6: gpu_ms={gpu_ms:.3f}")

    chain.dlss5_destroy(handle)

    if fails:
        for f in fails:
            print(f"[FAIL] {f}")
        print("FRAMEGEN INTERP TEST FAIL")
        return 1
    print("FRAMEGEN INTERP TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
