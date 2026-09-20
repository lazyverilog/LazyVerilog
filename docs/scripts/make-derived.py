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



def drop_lv_shadow(img):
    """Make the black shadow behind the LV letters transparent.

    Only the band holding the letters is touched: the otter above it and the outlined
    wordmark below it keep their dark pixels (the white "Lazy" needs its outline on a
    light page).  Coordinates are for the 512-px-high hero image.
    """
    px = img.load()
    for y in range(172, 384):
        limit = 60 if y < 185 else 95
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a and max(r, g, b) < limit:
                px[x, y] = (r, g, b, 0)


logo = Image.open(ROOT / "assets" / "lazyverilog_logo.png").convert("RGBA")
# Crop the transparent margin so the mark fills its box.
logo = logo.crop(logo.getbbox())

hero = logo.copy()
hero.thumbnail((512, 512), Image.LANCZOS)
drop_lv_shadow(hero)
hero.save(OUT / "logo.webp", "WEBP", quality=92, alpha_quality=100, method=6)

card = Image.new("RGBA", (1200, 630), MOCHA_BASE)
mark = logo.copy()
mark.thumbnail((560, 560), Image.LANCZOS)
card.alpha_composite(mark, ((1200 - mark.width) // 2, (630 - mark.height) // 2))
card.convert("RGB").save(OUT / "og.png", "PNG", optimize=True)

icon = Image.open(ROOT / "vscode" / "assets" / "lazyverilog_icon.png").convert("RGBA")
icon.save(OUT / "favicon.png", "PNG", optimize=True)

for name in ("logo.webp", "og.png", "favicon.png"):
    print(name, (OUT / name).stat().st_size, "bytes")
