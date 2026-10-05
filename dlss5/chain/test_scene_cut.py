#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# DLSS 5 regression test: scene-cut temporal history reset (ChainEngine).
#
# Field report: ghost trails + colour drift on scene cuts / camera switches
# because temporal history had no content-based reset. The engine now keeps
# a strided 64x36 luma thumb of the previous input; mean abs diff above the
# D5C_CUT_LIMIT/DLSS5_CUT_LIMIT threshold (default 0.15 ~= 38/255) drops
# history for that frame (first-frame path, histValid=0).
#
# This drives dlss5_process_frame with a real photo frame A x5, then A plus
# mild noise (must NOT trip), then a structurally different frame B (MUST
# trip exactly once), then more B (must not re-trip). Detection evidence is
# the engine stderr line "[chain] scene cut (meandiff=X.XXX) - history
# reset", gated by D5C_DEBUG_CUT=1 (set here before the DLL loads) and
# captured via an OS-level fd-2 redirect (ucrt fds are process-shared).
# A second phase with D5C_CUT_LIMIT=0 verifies the gate is disabled.
#
# Usage: python test_scene_cut.py [path-to-d5c_chain.dll]
# Exit: 0 = PASS, 1 = FAIL, 2 = SKIP (weights or DLL missing).
# ============================================================================
import os
import sys
import ctypes
import tempfile
from pathlib import Path
import numpy as np

# Engine env MUST be set before the DLL loads (ucrt takes its env snapshot
# at module init); the per-init getenv reads then see these values.
os.environ["D5C_DEBUG_CUT"] = "1"
os.environ.pop("D5C_CUT_LIMIT", None)          # phase 1 = default 0.15
os.environ.pop("DLSS5_CUT_LIMIT", None)

SCENE_CUT_TAG = "[chain] scene cut"


def load_bmp_bgra(path):
    """Minimal uncompressed-BMP loader -> (H, W, 4) uint8 BGRA array."""
    data = Path(path).read_bytes()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = int.from_bytes(data[10:14], "little")
    w = int.from_bytes(data[18:22], "little", signed=True)
    h = int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    comp = int.from_bytes(data[30:34], "little")
    if comp != 0 or bpp not in (24, 32):
        raise ValueError(f"{path}: need uncompressed 24/32bpp (bpp={bpp} comp={comp})")
    topdown = h < 0
    H, W = abs(h), w
    stride = ((W * bpp + 31) // 32) * 4
    img = np.empty((H, W, 4), dtype=np.uint8)
    for y in range(H):
        row = data[off + (y if topdown else H - 1 - y) * stride:
                   off + ((y if topdown else H - 1 - y) + 1) * stride]
        if bpp == 24:
            px = np.frombuffer(row[:W * 3], dtype=np.uint8).reshape(W, 3)
            img[y, :, 0] = px[:, 0]
            img[y, :, 1] = px[:, 1]
            img[y, :, 2] = px[:, 2]
        else:
            img[y] = np.frombuffer(row[:W * 4], dtype=np.uint8).reshape(W, 4)
        img[y, :, 3] = 255
    return img


def synth_scene_b(h, w, rng):
    """A structurally DIFFERENT scene (not just a brightness shift):
    saturated checker bands + orthogonal stripes + fresh noise field."""
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[:, :, 0] = (((xx // 37) + (yy // 23)) % 2 * 255)          # checkerboard B
    img[:, :, 1] = np.where((yy // 41) % 2 == 0, 230, 15)         # flat bands G
    img[:, :, 2] = np.where((xx // 53) % 2 == 0, 250, 30)         # stripes R
    img[:, :, 3] = 255
    img[:, :, :3] ^= rng.randint(0, 24, (h, w, 3)).astype(np.uint8)
    return img


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
        dll_path = None
        for c in (repo_root / "dlss5" / "m11d" / "build" / "Release" / "d5c_chain.dll",
                  repo_root / "dlss5" / "chain" / "build" / "Release" / "d5c_chain.dll",
                  repo_root / "dlss5" / "m11d" / "build-nmake" / "chain" / "d5c_chain.dll"):
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

    # ---- test frames ------------------------------------------------------
    frame_a = None
    for cand in (repo_root / "work" / "_m9b_cmp" / "_small512.bmp",
                 repo_root / "work" / "_m9b_cmp" / "native_crop.bmp"):
        if cand.exists():
            try:
                frame_a = load_bmp_bgra(cand)
                print(f"[TEST] frame A = {cand.name} ({frame_a.shape[1]}x{frame_a.shape[0]})")
                break
            except Exception as e:
                print(f"[WARN] {cand}: {e}")
    if frame_a is None:
        print("[SKIP] no usable BMP under work/_m9b_cmp")
        return 2
    h, w = frame_a.shape[:2]
    rng = np.random.RandomState(7)
    # A + mild per-pixel noise: mean|dLuma| ~1-3/255 ~ 0.01 << 0.15 -> no cut.
    noise = rng.randint(-4, 5, (h, w, 3))
    frame_an = frame_a.copy()
    frame_an[:, :, :3] = np.clip(frame_a[:, :, :3].astype(np.int16) + noise,
                                 0, 255).astype(np.uint8)
    frame_b = synth_scene_b(h, w, rng)

    # ---- stderr capture: fd 2 -> file (ucrt fds are process-shared, so the
    # DLL's fprintf(stderr) lands here too) ---------------------------------
    err_path = Path(tempfile.gettempdir()) / "d5c_scene_cut_stderr.log"
    err_fd = os.open(err_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    saved_fd2 = os.dup(2)
    os.dup2(err_fd, 2)

    fails = []
    try:
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

        out = np.empty_like(frame_a)
        out_ptr = out.ctypes.data_as(ctypes.c_void_p)

        def run(ctx, img, fi, tag):
            ret = chain.dlss5_process_frame(ctx, w, h, img.tobytes(), out_ptr,
                                            ctypes.c_long(fi))
            if ret != 0:
                fails.append(f"{tag}: dlss5_process_frame returned {ret}")
            return ret

        def count_cuts():
            try:
                return Path(err_path).read_bytes().count(SCENE_CUT_TAG.encode())
            except OSError:
                return 0

        print("[TEST] phase 1: default threshold (D5C_CUT_LIMIT unset -> 0.15)")
        ctx = chain.dlss5_create(str(weights_path).encode(),
                               (str(shader_dir) + "\\").encode(), 1.0, 1.0, 1.0)
        if not ctx:
            print("[FAIL] dlss5_create returned NULL")
            return 1
        fi = 0
        marks = {}
        for tag, img, n in (("A", frame_a, 5), ("A+noise", frame_an, 3),
                            ("B(cut)", frame_b, 1), ("B", frame_b, 2)):
            before = count_cuts()
            for _ in range(n):
                fi += 1
                run(ctx, img, fi, tag)
            got = count_cuts() - before
            marks[tag] = got
            print(f"[TEST]   {tag} x{n}: scene-cut lines = {got}")
        chain.dlss5_destroy(ctx)

        if marks.get("A", 0) != 0:
            fails.append(f"static A sequence fired {marks['A']} cut(s)")
        if marks.get("A+noise", 0) != 0:
            fails.append(f"mild-noise sequence fired {marks['A+noise']} cut(s)")
        if marks.get("B(cut)", 0) != 1:
            fails.append(f"scene-change frame fired {marks.get('B(cut)')} cut(s), expected 1")
        if marks.get("B", 0) != 0:
            fails.append(f"post-cut B frames fired {marks['B']} extra cut(s)")

        print("[TEST] phase 2: D5C_CUT_LIMIT=0 -> gate disabled")
        os.environ["D5C_CUT_LIMIT"] = "0"
        ctx = chain.dlss5_create(str(weights_path).encode(),
                                 (str(shader_dir) + "\\").encode(), 1.0, 1.0, 1.0)
        if not ctx:
            fails.append("phase2: dlss5_create returned NULL")
        else:
            before = count_cuts()
            fi += 1
            run(ctx, frame_a, fi, "off/A")
            fi += 1
            run(ctx, frame_b, fi, "off/B")   # would be a cut if the gate ran
            got = count_cuts() - before
            print(f"[TEST]   disabled gate on real scene change: cuts = {got}")
            if got != 0:
                fails.append(f"D5C_CUT_LIMIT=0 still fired {got} cut(s)")
            chain.dlss5_destroy(ctx)
    finally:
        os.dup2(saved_fd2, 2)
        os.close(saved_fd2)
        os.close(err_fd)

    # ---- captured engine stderr (evidence) ---------------------------------
    try:
        lines = [l for l in Path(err_path).read_text(errors="replace").splitlines()
                 if SCENE_CUT_TAG in l]
        print(f"[TEST] captured stderr cut lines ({len(lines)}):")
        for l in lines:
            print("       " + l)
    except OSError:
        print("[WARN] could not read captured stderr log")

    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("SCENE CUT TEST FAIL")
        return 1
    print("SCENE CUT TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
