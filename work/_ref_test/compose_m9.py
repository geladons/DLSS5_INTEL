# compose_m9.py — compose gpu head to PNG, compare with reference output.
import pathlib

import numpy as np
from PIL import Image

ROOT = pathlib.Path("C:/Users/AI/Desktop/DLSS5_INTEL/work/_ref_test")
GPU = pathlib.Path("C:/Users/AI/Desktop/DLSS5_INTEL/dlss5/m9-unet/build/Release/out")

src = np.asarray(Image.open(ROOT / "photo_512.png").convert("RGB"), dtype=np.float32) / 255.0


def compose(head):
    residual = head[..., :3].astype(np.float16).astype(np.float32) * np.float32(0.25)
    return np.clip(src + residual, 0, 1)


head_g = np.fromfile(GPU / "gpu_head.bin", dtype=np.float32).reshape(320, 512, 16)[..., :4]
head_r = np.fromfile(ROOT / "golden_head.bin", dtype=np.float32).reshape(320, 512, 4)

img_g = compose(head_g)
img_r = compose(head_r)

ref_png = np.asarray(Image.open(ROOT / "ref_out.png").convert("RGB"), dtype=np.float32) / 255.0

d = np.abs(img_g - img_r)
print("gpu-compose vs golden-compose: max %.4f mean %.5f" % (d.max(), d.mean()))
d2 = np.abs(img_g - ref_png)
print("gpu-compose vs ref_out.png:  max %.4f mean %.5f" % (d2.max(), d2.mean()))
mse = ((img_g - ref_png) ** 2).mean()
print("PSNR vs ref_out.png: %.2f dB" % (10 * np.log10(1.0 / max(mse, 1e-12))))

Image.fromarray((img_g * 255).round().astype(np.uint8)).save(GPU / "m9_out.png")
# side-by-side: source | gpu | ref
sbs = np.concatenate([src, img_g, ref_png], axis=1)
Image.fromarray((sbs * 255).round().astype(np.uint8)).save(GPU / "m9_sbs.png")
print("saved m9_out.png / m9_sbs.png")
