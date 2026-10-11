"""Silkscreen cat portrait: a black-and-white PNG turned into one board-only footprint of
filled silk rectangles (one footprint, so its pixels cannot trip the silk-overlap check)."""
import uuid
from pathlib import Path

from PIL import Image

from board.sexpr import Sym

S = Sym
MIN_FEATURE = 0.15     # JLCPCB's smallest reliable silkscreen feature, mm
_NS = uuid.UUID("5a1d101c-0000-4000-8000-000000000102")


def pixel_rects(img):
    """Run-length the set pixels per row, then merge identical runs down the rows.
    Returns (x0, x1, y0, y1) in pixel units, x1/y1 exclusive."""
    w, h = img.size
    px = img.load()
    open_runs, rects = {}, []
    for y in range(h + 1):
        runs = set()
        if y < h:
            x = 0
            while x < w:
                if px[x, y]:
                    x0 = x
                    while x < w and px[x, y]:
                        x += 1
                    runs.add((x0, x))
                else:
                    x += 1
        for r in list(open_runs):
            if r not in runs:
                rects.append((r[0], r[1], open_runs.pop(r), y))
        for r in runs:
            open_runs.setdefault(r, y)
    return rects


def load_art(png, height_mm):
    """(1-bit image, mm per pixel): downsampled so that every pixel is at least MIN_FEATURE."""
    img = Image.open(png).convert("L")
    rows = max(1, int(height_mm / MIN_FEATURE))
    if rows < img.height:
        img = img.resize((max(1, round(img.width * rows / img.height)), rows), Image.LANCZOS)
    return img.point(lambda v: 255 if v >= 128 else 0).convert("1"), height_mm / img.height


def art_size(png, height_mm):
    """(width, height) in mm that the portrait will occupy."""
    img, px = load_art(png, height_mm)
    return img.width * px, img.height * px


def art_footprint(png, x, y, height_mm, name="ART", side="front"):
    """Footprint node whose image occupies the box with top-left corner at board (x, y) in
    top-view coordinates, 'height_mm' tall.

    side="back" puts the pixels on B.SilkS and mirrors them left-right, so that the picture reads
    correctly when the board is turned over (the viewer's left is then the board's +x). KiCad
    stores a back footprint's graphics already flipped, so the mirrored local coordinates are
    written as they are."""
    if side not in ("front", "back"):
        raise ValueError("side must be front or back")
    back = side == "back"
    img, px = load_art(Path(png), height_mm)
    width = img.width * px
    n = [0]

    def uid():
        n[0] += 1
        return [S("uuid"), str(uuid.uuid5(_NS, f"{name}{x}{y}{n[0]}"))]

    silk = "B.SilkS" if back else "F.SilkS"
    fp = [S("footprint"), name, [S("layer"), "B.Cu" if back else "F.Cu"], uid(), [S("at"), x, y],
          [S("attr"), S("board_only"), S("exclude_from_pos_files"), S("exclude_from_bom")],
          [S("fp_text"), S("reference"), name, [S("at"), 0, 0], [S("layer"), silk], S("hide"),
           [S("effects"), [S("font"), [S("size"), 0.8, 0.8], [S("thickness"), 0.12]]]
           + ([[S("justify"), S("mirror")]] if back else [])]]
    for x0, x1, y0, y1 in pixel_rects(img):
        a, b, c, d = (round(v * px, 4) for v in (x0, x1, y0, y1))
        if back:                                   # mirror about the box's vertical centre line
            a, b = round(width - b, 4), round(width - a, 4)
        fp.append([S("fp_poly"), [S("pts"), [S("xy"), a, c], [S("xy"), b, c], [S("xy"), b, d], [S("xy"), a, d]],
                   [S("stroke"), [S("width"), 0], [S("type"), S("solid")]], [S("fill"), S("solid")],
                   [S("layer"), silk], uid()])
    return fp
