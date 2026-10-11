# Mu's silkscreen portrait: art/mu.png (1-bit, white = silk on the black board).
#
# Source: Martin's Photos library, Mu face-on with her ears up (28 Dec 2024), asset
# 1C84AAA9-2F12-493F-A9E1-88D410785A10 (local 768x1024 preview). Her outline comes from macOS
# Vision's subject lift, so nothing is hand-drawn:
#
#   swift art/liftsubject.swift <photo.jpeg> /tmp/mu_mask.png
#   python3 art/make_mu.py <photo.jpeg> /tmp/mu_mask.png art/mu.png
#
# Inside the outline, local contrast keeps what's darker than its surroundings (eyes, nose, mouth,
# whisker pads, fur streaks) as bare black board. Martin picked k = 20 over 12 and a global mix.
import sys

import numpy as np
from PIL import Image, ImageFilter, ImageOps

photo_path, mask_path, out_path = sys.argv[1:4]
WIDTH = 112                # pixels; build.py places them at 0.16 mm, so ~18 mm wide
CROP = (100, 0, 700, 690)  # head, ears and ruff
K = 20                     # how much darker than the local average counts as a feature
BLUR = 10                  # local-average radius, at 3x the final size

photo = ImageOps.autocontrast(Image.open(photo_path).convert("L").crop(CROP), cutoff=1)
mask = Image.open(mask_path).convert("L").crop(CROP)
height = round(photo.height * WIDTH / photo.width)

big = photo.resize((WIDTH * 3, height * 3), Image.LANCZOS)
lum = np.asarray(big).astype(float)
local = np.asarray(big.filter(ImageFilter.GaussianBlur(BLUR))).astype(float)
inside = np.asarray(mask.resize((WIDTH * 3, height * 3), Image.LANCZOS)) > 127

art = inside & ~(lum < local - K)
img = Image.fromarray((art * 255).astype(np.uint8)).resize((WIDTH, height), Image.LANCZOS)
img = img.point(lambda v: 255 if v > 127 else 0).filter(ImageFilter.MedianFilter(3))
img.point(lambda v: 255 if v > 127 else 0).convert("1").save(out_path)
print(f"{out_path}: {WIDTH} x {height} px")
