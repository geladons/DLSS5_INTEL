#!/usr/bin/env python3
# ============================================================================
# DLSS 5: render-scale reconstruction test (Task E).
# The scaled path (<1.0) must reconstruct detail: final = native_full +
# upscale(compose_low - source_low) instead of presenting a bilinear blow-up
# of the low-res compose image.
#  T1 scale 0.5 keeps >=70% of input high-frequency energy (stripes, checker).
#  T2 discrimination vs the pre-change production DLL (env D5C_OLD_DLL;
#     printed only - the assert is on the new DLL).
#  T3 scale 1.0 output is byte-identical to the old production DLL.
#  T4 the residual is actually applied at 0.5 (nonzero delta vs native).
# ============================================================================
import os
import sys
import ctypes
import shutil
import tempfile
from pathlib import Path
import numpy as np

W, H = 640, 384

# ROI boxes: (x0, y0, x1, y1) - inside the 32px border
STRIPES = (60, 60, 260, 180)     # 200x120, 1-px vertical stripes 40/215
CHECKER = (340, 60, 540, 180)    # 200x120, 2-px checker 40/215


def make_frame(w, h):
    img = np.zeros((h, w, 4), dtype=np.uint8)
    yy, xx = np.mgrid[0:h, 0:w]
    img[:, :, 0] = ((xx / w) * 100 + 40).astype(np.uint8)          # B gradient
    img[:, :, 1] = ((yy / h) * 100 + 40).astype(np.uint8)          # G gradient
    img[:, :, 2] = np.full((h, w), 70, dtype=np.uint8)             # R flat
    img[:, :, 3] = 255
    x0, y0, x1, y1 = STRIPES
    img[y0:y1, x0:x1, :3] = np.where((np.arange(x1 - x0) % 2 == 0)[None, :, None],
                                     215, 40).astype(np.uint8)
    x0, y0, x1, y1 = CHECKER
    gx, gy = np.meshgrid(np.arange(x1 - x0), np.arange(y1 - y0))
    img[y0:y1, x0:x1, :3] = np.where(((gx // 2 + gy // 2) % 2 == 0)[..., None],
                                     215, 40).astype(np.uint8)
    img[:32, :, :] = 0
    img[-32:, :, :] = 0
    img[:, :32, :] = 0
    img[:, -32:, :] = 0
    img[:, :, 3] = 255
    return img


def hf(img, roi):
    """Mean |4*c - up - dn - lt - rt| over the ROI (per-channel, uint8 float)."""
    x0, y0, x1, y1 = roi
    f = img.astype(np.float32)[..., :3]
    c = f[y0 + 1:y1 - 1, x0 + 1:x1 - 1]
    lap = (4.0 * c - f[y0:y1 - 2, x0 + 1:x1 - 1] - f[y0 + 2:y1, x0 + 1:x1 - 1]
           - f[y0 + 1:y1 - 1, x0:x1 - 2] - f[y0 + 1:y1 - 1, x0 + 2:x1])
    return float(np.abs(lap).mean())


def load_chain(dll_path):
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
    return chain


def process(chain, weights, shaders, scale, frame, w, h):
    handle = chain.dlss5_create(weights, shaders, scale, 1.0, 1.0)
    if not handle:
        return None
    out = np.zeros_like(frame)
    ret = chain.dlss5_process_frame(handle, w, h,
                                    frame.ctypes.data_as(ctypes.c_void_p),
                                    out.ctypes.data_as(ctypes.c_void_p), 1)
    chain.dlss5_destroy(handle)
    return out if ret == 0 else None


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
    print(f"[TEST] new DLL: {dll_path}")

    weights = str(repo_root / "work" / "mlxw" / "dlssnr-logical.safetensors").encode("utf-8")
    shaders = (str(repo_root / "dlss5" / "m11d" / "build" / "Release") + "\\").encode("utf-8")

    chain = load_chain(dll_path)
    frame = make_frame(W, H)
    fails = []

    print(f"[TEST] processing at scale 1.0 and 0.5 ({W}x{H})...")
    out10 = process(chain, weights, shaders, 1.0, frame, W, H)
    out05 = process(chain, weights, shaders, 0.5, frame, W, H)
    if out10 is None or out05 is None:
        print("[FAIL] engine init/process failed")
        return 1

    # ---- T1: high-frequency detail preserved at scale 0.5 --------------------
    in_s, in_c = hf(frame, STRIPES), hf(frame, CHECKER)
    out_s, out_c = hf(out05, STRIPES), hf(out05, CHECKER)
    rs, rc = out_s / in_s, out_c / in_c
    print(f"[T1] hf ratio stripes={rs:.3f} checker={rc:.3f} (need >= 0.7)")
    if rs < 0.7 or rc < 0.7:
        fails.append(f"T1: detail lost stripes={rs:.3f} checker={rc:.3f}")

    # ---- T2/T3: old production DLL comparison --------------------------------
    old_dll = os.environ.get("D5C_OLD_DLL")
    old10 = None
    if old_dll and os.path.exists(old_dll):
        # Same-basename DLLs collide in Windows module dedup - copy aside.
        tmp = os.path.join(tempfile.gettempdir(), "d5c_chain_oldref.dll")
        shutil.copyfile(old_dll, tmp)
        old_shaders = os.path.join(os.path.dirname(old_dll), "shaders") + "\\"
        if not os.path.isdir(old_shaders):
            old_shaders = os.path.dirname(old_dll) + "\\"
        print(f"[TEST] old DLL: {old_dll} (shaders: {old_shaders})")
        old = load_chain(tmp)
        old05 = process(old, weights, old_shaders.encode("utf-8"), 0.5, frame, W, H)
        old10 = process(old, weights, old_shaders.encode("utf-8"), 1.0, frame, W, H)
        if old05 is not None:
            o_s, o_c = hf(old05, STRIPES), hf(old05, CHECKER)
            print(f"[T2] OLD dll hf ratio stripes={o_s / in_s:.3f} "
                  f"checker={o_c / in_c:.3f} (expect well below 0.7)")
        else:
            print("[T2] old dll process failed at 0.5")
    else:
        print("[T2/T3] D5C_OLD_DLL not set/missing - old-DLL checks skipped")

    if old10 is not None:
        same = bool(np.array_equal(out10, old10))
        print(f"[T3] scale 1.0 byte-identical to old dll: {same}")
        if not same:
            fails.append("T3: native path output changed")
    else:
        print("[T3] skipped (no old DLL output)")

    # ---- T4: residual actually applied at 0.5 --------------------------------
    f32 = frame.astype(np.float32)
    m05 = float(np.abs(out05.astype(np.float32) - f32)[..., :3].mean())
    m10 = float(np.abs(out10.astype(np.float32) - f32)[..., :3].mean())
    ratio = m05 / m10 if m10 > 0 else 0.0
    print(f"[T4] mean|out0.5-in|={m05:.2f} mean|out1.0-in|={m10:.2f} "
          f"ratio={ratio:.3f} (need >0.5 and 0.33<=ratio<=3.0)")
    if not (m05 > 0.5 and 0.33 <= ratio <= 3.0):
        fails.append(f"T4: residual missing/mis-scaled m05={m05:.2f} ratio={ratio:.3f}")

    if fails:
        for f in fails:
            print(f"[FAIL] {f}")
        print("SCALE RECONSTRUCTION TEST FAIL")
        return 1
    print("SCALE RECONSTRUCTION TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
