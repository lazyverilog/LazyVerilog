"""Regenerate the derived site images in docs/public/ from the untouched originals.

One-off dev tool, not part of any CI job.  Needs Pillow:

    python -m venv .venv && .venv/bin/pip install pillow==12.3.0
    .venv/bin/python docs/scripts/make-derived.py

Originals stay the source of truth:
    assets/lazyverilog_logo.png           -> logo.webp, og.png
    vscode/assets/lazyverilog_icon.png    -> favicon.png
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "public"
OUT.mkdir(parents=True, exist_ok=True)

# Catppuccin Mocha "base", the background of the social card.
MOCHA_BASE = (0x1E, 0x1E, 0x2E, 255)

def drop_dark_plate(img):
    """Make the black plate behind the LV letters and the "LazyVerilog" wordmark transparent.

    Only the part below the otter is touched, so its eyes and nose keep their dark pixels.  The
    rows are for the 512-px-high hero image.  Faint dark fringe pixels go too, so no line is left
    where the plate ended.
    """
    px = img.load()
    for y in range(172, img.height):
        limit = 60 if y < 185 else 95
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a and (max(r, g, b) < limit or (a < 64 and max(r, g, b) < 150)):
                px[x, y] = (r, g, b, 0)

def recolor_lazy(img):
    """Paint the white "Lazy" the purple of "Verilog", so the wordmark reads on a light page.

    The white letters lost their black outline with the plate.  They are the grey-white pixels
    (low saturation) of the wordmark rows; "Verilog" is the saturated ones.
    """
    px = img.load()
    rows = range(385, img.height)
    purple = sorted(
        (px[x, y][:3] for y in rows for x in range(img.width)
         if px[x, y][3] == 255 and max(px[x, y][:3]) - min(px[x, y][:3]) >= 80),
        key=sum,
    )
    r, g, b = purple[len(purple) // 2]                 # the median: one flat colour, no outliers
    for y in rows:
        for x in range(img.width):
            pr, pg, pb, a = px[x, y]
            if a and max(pr, pg, pb) - min(pr, pg, pb) < 60:
                px[x, y] = (r, g, b, a)


logo = Image.open(ROOT / "assets" / "lazyverilog_logo.png").convert("RGBA")
# Crop the transparent margin so the mark fills its box.
logo = logo.crop(logo.getbbox())

hero = logo.copy()
hero.thumbnail((512, 512), Image.LANCZOS)
drop_dark_plate(hero)
recolor_lazy(hero)
hero.save(OUT / "logo.webp", "WEBP", quality=92, alpha_quality=100, method=6)

card = Image.new("RGBA", (1200, 630), MOCHA_BASE)
mark = hero.resize((round(hero.width * 560 / hero.height), 560), Image.LANCZOS)
card.alpha_composite(mark, ((1200 - mark.width) // 2, (630 - mark.height) // 2))
card.convert("RGB").save(OUT / "og.png", "PNG", optimize=True)

icon = Image.open(ROOT / "vscode" / "assets" / "lazyverilog_icon.png").convert("RGBA")
icon.save(OUT / "favicon.png", "PNG", optimize=True)

for name in ("logo.webp", "og.png", "favicon.png"):
    print(name, (OUT / name).stat().st_size, "bytes")
