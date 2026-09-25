# Generate icon.ico: the DLSS5 diamond logo (accent on deep-navy).
import os
from PIL import Image, ImageDraw

BG = (13, 16, 23, 255)        # #0d1017
ACCENT = (91, 140, 255, 255)  # #5b8cff
ACCENT2 = (154, 107, 255, 255)

def draw(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = max(1, size // 16)                    # margin
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=size // 5,
                        fill=BG)
    c = size / 2.0
    r = c - m                                 # outer diamond radius
    d.polygon([(c, c - r), (c + r, c), (c, c + r), (c - r, c)], fill=ACCENT)
    r2 = r * 0.45                             # inner cutout
    d.polygon([(c, c - r2), (c + r2, c), (c, c + r2), (c - r2, c)], fill=BG)
    return img

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.ico")
base = draw(256)
base.save(out, format="ICO", sizes=[(16, 16), (24, 24), (32, 32),
                                    (48, 48), (64, 64), (128, 128),
                                    (256, 256)])
print("wrote", out, os.path.getsize(out), "bytes")
