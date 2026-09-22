# dump_torch.py — dump golden tensors for m9-unet validation.
# Runs the reference model on photo_512.png (extent 512x320, identity geometry),
# captures features + every scale boundary as fp32 NHWC .bin files + shapes json.
import json
import pathlib

import numpy as np
import torch
from PIL import Image

import mlxdlss.model as M
from mlxdlss.pipeline import NeuralRenderingPipeline

ROOT = pathlib.Path(__file__).parent
WEIGHTS = ROOT.parent / "mlxw" / "dlssnr-logical.safetensors"

import sys
IMG = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "photo_512.png"
OUT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT
OUT.mkdir(parents=True, exist_ok=True)

caps = {}


def wrap_fn(name):
    orig = getattr(M, name)

    def wrapper(*a, **k):
        out = orig(*a, **k)
        caps[name] = out
        return out

    setattr(M, name, wrapper)


def wrap_method(name, key_prefix):
    orig = getattr(M.NeuralRenderingModel, name)

    def wrapper(self, value, index, *a, **k):
        out = orig(self, value, index, *a, **k)
        caps[f"{key_prefix}{index}"] = out
        return out

    setattr(M.NeuralRenderingModel, name, wrapper)


def wrap_up():
    orig = M.NeuralRenderingModel._upsample_window

    def wrapper(self, value, skip, index, *a, **k):
        out = orig(self, value, skip, index, *a, **k)
        caps[f"up{index}"] = out
        return out

    M.NeuralRenderingModel._upsample_window = wrapper


wrap_method("_window", "w")
wrap_method("_split_window", "w")
wrap_method("_global", "g")
wrap_method("_downsample_window", "ds")
wrap_up()
wrap_fn("downsample")          # b30 bridge (only call site)
wrap_fn("decoder_input_merge") # b39 merge
wrap_fn("_rows")               # merged70

img = np.asarray(Image.open(IMG).convert("RGB"), dtype=np.float32) / 255.0
pipe = NeuralRenderingPipeline.from_safetensors(WEIGHTS, device="cpu", precision="reference")
prepared = pipe.prepare(img, profile="standard", processing_scale=1.0, frame_index=0)
feats = prepared.features  # (H, W, 16) fp32
print("features", feats.shape)
head = pipe.run_features(feats)
caps["head"] = torch.from_numpy(head)

shapes = {}


def dump(name, t):
    if isinstance(t, np.ndarray):
        arr = t
    else:
        arr = t.detach().to(torch.float32).cpu().numpy()
    arr = np.ascontiguousarray(arr, dtype=np.float32)
    if arr.ndim == 4:  # drop batch
        arr = arr[0]
    shapes[name] = list(arr.shape)
    arr.tofile(OUT / f"golden_{name}.bin")


dump("features", feats)
for name in sorted(caps):
    if name == "features":
        continue
    dump(name, caps[name])

(OUT / "golden_shapes.json").write_text(json.dumps(shapes, indent=1))
print("dumped:", ", ".join(sorted(shapes)))
