"""Everything that decides how a recording looks.  Change it here, re-run, and every
image and GIF comes out at the same size and in the same theme."""
import os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEPS = HERE / ".deps"
OUT = HERE / "out"
PROJECT = HERE / "demo-proj"

# -- Terminal grid and pixels --------------------------------------------------
COLS, ROWS = 100, 26          # Neovim's screen, in cells.  GIFs come out 872 x 505 px.
FONT_PX = 14                  # GIF font size in CSS pixels
SHOT_FONT_PX = 15             # still images use a slightly larger font
LINE_HEIGHT = 1.3
CELL_W = 0.6                  # cell width as a fraction of FONT_PX (monospace)
PAD = 16                      # horizontal page padding inside the render
GIF_MARGIN = 32               # window size = grid size + margin (GIF)
SHOT_MARGIN = 40              # ... and for still images
FONT_FAMILY = "'Cascadia Mono',Consolas,monospace"
GIF_SCALE = 1                 # device scale factor for GIFs
SHOT_SCALE = 2                # device scale factor for still images
GIF_COLORS = 96               # one shared palette per GIF, no dithering

# -- Timing (milliseconds) -----------------------------------------------------
TYPE_MS = 60                  # per character of a typed ex command
HOLD_MS = 1500                # default pause on a finished state

# -- Theme ---------------------------------------------------------------------
THEME_REPO = "https://github.com/catppuccin/nvim"   # the flavour (mocha) is set in init.lua


# -- Tools; override with environment variables --------------------------------
def _first(*candidates):
    for c in candidates:
        if c and (Path(c).exists() or shutil.which(c)):
            return c
    return None


NVIM = _first(os.environ.get("NVIM"), "nvim") or "nvim"        # Neovim 0.11 or newer
LSP = os.environ.get("LV_LSP")                                 # unset: the plugin finds or downloads it
CHROME = _first(
    os.environ.get("CHROME"),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "google-chrome", "chromium", "chromium-browser",
)


def theme_dir():
    d = DEPS / "catppuccin-nvim"
    if not d.exists():
        print("cloning", THEME_REPO, "->", d, file=sys.stderr)
        subprocess.run(["git", "clone", "--depth", "1", THEME_REPO, str(d)], check=True)
    return d
