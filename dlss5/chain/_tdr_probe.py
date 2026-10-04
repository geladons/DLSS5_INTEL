#!/usr/bin/env python3
# TDR probe: does the shipping chain engine die on a full-res frame?
# Loads d5c_chain.dll and processes one 2560x1440 frame at scale 1.0 -
# the same giant-grid extent that device-losts m8b-live's monolithic submit.
# Prints progress per step; a hang or VK error in process_frame = BUG CONFIRMED.
import sys, ctypes, time
from pathlib import Path
import numpy as np

repo = Path(__file__).resolve().parents[2]
weights = repo / "work" / "mlxw" / "dlssnr-logical.safetensors"
dll = repo / "dlss5" / "m11d" / "build" / "Release" / "d5c_chain.dll"
if not dll.exists():
    dll = repo / "dlss5" / "chain" / "build" / "Release" / "d5c_chain.dll"
print(f"dll={dll}", flush=True)

chain = ctypes.CDLL(str(dll))
chain.dlss5_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_float]
chain.dlss5_create.restype = ctypes.c_void_p
chain.dlss5_set_scale.argtypes = [ctypes.c_void_p, ctypes.c_float]
chain.dlss5_process_frame.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p]
chain.dlss5_process_frame.restype = ctypes.c_int
chain.dlss5_destroy.argtypes = [ctypes.c_void_p]

shader_dir = str(dll.parent).encode() + b"\\"
h = chain.dlss5_create(str(weights).encode(), shader_dir, 1.0, 1.0, 1.0)
print(f"create -> {h:#x}", flush=True)
if not h:
    sys.exit(2)
import os
chain.dlss5_set_scale(h, float(os.environ.get("SC", "1.0")))

W, H = 2560, 1440
frame_in = np.random.randint(0, 256, (H, W, 4), dtype=np.uint8)
frame_out = np.empty_like(frame_in)
print(f"processing {W}x{H} @ scale 1.0 ...", flush=True)
for i in range(5):
    t0 = time.time()
    ret = chain.dlss5_process_frame(h, W, H,
                                    frame_in.ctypes.data, frame_out.ctypes.data,
                                    i, None)
    print(f"frame {i}: ret={ret} in {time.time()-t0:.2f}s", flush=True)
    if ret != 0:
        print("NONZERO RETURN - device error", flush=True)
        break
print("done", flush=True)
chain.dlss5_destroy(h)
