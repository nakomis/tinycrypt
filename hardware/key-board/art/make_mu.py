# Mu portrait: photo 4 (face close-up) thresholded, inside a head-and-ears outline drawn by hand.
from PIL import Image, ImageOps, ImageFilter, ImageDraw

SRC = ("/Users/nakomis/Pictures/Photos Library.photoslibrary/resources/derivatives/B/"
       "B44E7CE6-77F9-43FE-BCE9-5587FBBEDAC2_1_105_c.jpeg")
W, H = 112, 128
FACE_W, FACE_TOP = 104, 20

im = Image.open(SRC).convert("L").crop((0, 200, 768, 1000))
im = ImageOps.autocontrast(im, cutoff=2).filter(ImageFilter.UnsharpMask(radius=4, percent=180, threshold=2))
fh = round(im.height * FACE_W / im.width)
face = im.resize((FACE_W, fh), Image.LANCZOS)

for thr in (100, 115, 130):
    canvas = Image.new("L", (W, H), 0)
    bw = face.point(lambda v, t=thr: 255 if v > t else 0)
    canvas.paste(bw, ((W - FACE_W) // 2, FACE_TOP))

    # Head mask: a broad rounded face plus two pointed, slightly tufted ears (Maine Coon-ish).
    mask = Image.new("L", (W, H), 0)
    m = ImageDraw.Draw(mask)
    m.ellipse((4, 30, W - 4, H - 2), fill=255)
    m.polygon([(10, 58), (14, 2), (46, 34)], fill=255)          # left ear
    m.polygon([(W - 10, 58), (W - 14, 2), (W - 46, 34)], fill=255)  # right ear
    # Forehead between the ears, filled solid so the top of the head is continuous.
    m.rectangle((30, 30, W - 30, 50), fill=255)
    art = Image.composite(canvas, Image.new("L", (W, H), 0), mask)

    d = ImageDraw.Draw(art)
    # Above the photo the canvas is empty: fill the ears and forehead with fur...
    top = Image.new("L", (W, H), 0)
    ImageDraw.Draw(top).rectangle((0, 0, W, 34), fill=255)
    art = Image.composite(Image.composite(Image.new("L", (W, H), 255), art, top), Image.new("L", (W, H), 0), mask)
    d = ImageDraw.Draw(art)
    # ...with dark inner ears, and keep tufts at the tips.
    d.polygon([(20, 40), (20, 16), (36, 32)], fill=0)
    d.polygon([(W - 20, 40), (W - 20, 16), (W - 36, 32)], fill=0)

    art = art.filter(ImageFilter.MedianFilter(3)).point(lambda v: 255 if v > 127 else 0)
    art.convert("1").save(f"/private/tmp/claude-501/photos/mu_head_{thr}.png")

# Preview: the three thresholds, white silk on black mask, 4x.
tiles = [Image.open(f"/private/tmp/claude-501/photos/mu_head_{t}.png").convert("RGB").resize((W * 4, H * 4), Image.NEAREST)
         for t in (100, 115, 130)]
sheet = Image.new("RGB", (sum(t.width for t in tiles) + 40, H * 4 + 24), (12, 12, 12))
d = ImageDraw.Draw(sheet)
x = 0
for t, thr in zip(tiles, (100, 115, 130)):
    sheet.paste(t, (x, 22))
    d.text((x + 4, 4), f"threshold {thr}", fill=(220, 200, 0))
    x += t.width + 20
sheet.save("/private/tmp/claude-501/photos/mu_head_options.png")
