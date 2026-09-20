"""Scripted recording of a real Neovim session: GIFs (frames rendered by headless Chrome)
and still images."""
import re, shutil, tempfile
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

import settings as S
from nvrender import Nv, chrome_shot


class Rec:
    def __init__(self, name, file, cols=S.COLS, rows=S.ROWS):
        S.OUT.mkdir(exist_ok=True)
        self.name, self.cols, self.rows = name, cols, rows
        self.nv = Nv(file, cols=cols, rows=rows)
        self.nv.settle(1.5)
        self.nv.wait_lsp()
        self.nv.settle(4, 20)                      # let the project index and the first parse land
        self.frames = []                           # (html, milliseconds)

    # -- building blocks -------------------------------------------------------
    def snap(self, ms):
        self.frames.append((self.nv.html(), ms))

    def hold(self, ms=S.HOLD_MS):
        self.snap(ms)

    def keys(self, k, ms=500, settle=0.5, maxw=8):
        """Send keys, wait for the screen to settle, then show the result for `ms`."""
        self.nv.keys(k)
        self.nv.settle(settle, maxw)
        self.snap(ms)

    def type(self, text, ms=90, settle=0.12):
        """Type text one character per frame."""
        for ch in text:
            self.nv.keys("<lt>" if ch == "<" else ch)
            self.nv.settle(settle, 3)
            self.snap(ms)

    def cmd(self, text, run_ms=1200, settle=1.5, maxw=15):
        """Type an ex command where the viewer can see it, then run it."""
        self.nv.keys(":")
        self.nv.settle(0.2, 3)
        self.type(text, ms=S.TYPE_MS, settle=0.08)
        self.hold(300)
        self.nv.keys("<CR>")
        self.nv.settle(settle, maxw)
        self.snap(run_ms)

    def cursor(self, line, col=0):
        self.nv.cmd(f"call cursor({line},{col})")
        self.nv.settle(0.4, 3)

    def code_action(self, title, hold=2200):
        """Open the LSP code action menu (`gra`), pick the entry containing `title`."""
        self.nv.keys("gra")
        self.nv.settle(1.5, 8)
        num = None
        for row in self.nv.grid:
            m = re.match(r"\s*(\d+): (.*)", "".join(c[0] for c in row))
            if m and title in m.group(2):
                num = m.group(1)
                break
        assert num, "code action not offered: " + title
        self.snap(hold)
        self.type(num, ms=700, settle=0.3)
        self.nv.keys("<CR>")
        self.nv.settle(2.0, 10)
        self.snap(1800)

    # -- output ----------------------------------------------------------------
    def shot(self, out_name=None):
        """Save the current screen as a still PNG at SHOT_SCALE."""
        out = S.OUT / ((out_name or self.name) + ".png")
        tmp = tempfile.mkdtemp(prefix="rec")
        size = self.nv.window_size(S.SHOT_FONT_PX, S.SHOT_MARGIN)
        chrome_shot(self.nv.html(S.SHOT_FONT_PX), out, size, S.SHOT_SCALE, tmp)
        shutil.rmtree(tmp, ignore_errors=True)
        self.nv.close()
        print(out.name, Image.open(out).size)

    def finish(self):
        """Save the frames as a GIF."""
        self.nv.close()
        merged = []                                # identical neighbours become one longer frame
        for h, ms in self.frames:
            if merged and merged[-1][0] == h:
                merged[-1][1] += ms
            else:
                merged.append([h, ms])
        tmp = tempfile.mkdtemp(prefix="rec")
        size = self.nv.window_size()

        def render(i):
            png = f"{tmp}/{i}.png"
            chrome_shot(merged[i][0], png, size, S.GIF_SCALE, tmp)
            return Image.open(png).convert("RGB")

        with ThreadPoolExecutor(6) as ex:
            imgs = list(ex.map(render, range(len(merged))))
        shutil.rmtree(tmp, ignore_errors=True)
        # one palette shared by every frame, built from a sample of them
        sample = imgs[:: max(1, len(imgs) // 6)][:6]
        sheet = Image.new("RGB", (imgs[0].width, imgs[0].height * len(sample)))
        for k, im in enumerate(sample):
            sheet.paste(im, (0, k * imgs[0].height))
        pal = sheet.quantize(colors=S.GIF_COLORS, method=Image.Quantize.MEDIANCUT)
        q = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in imgs]
        path = S.OUT / (self.name + ".gif")
        q[0].save(path, save_all=True, append_images=q[1:], loop=0, optimize=True, disposal=1,
                  duration=[max(40, int(m)) for _, m in merged])
        imgs[-1].save(S.OUT / (self.name + "-last.png"))   # the final frame, for a quick look
        print(path.name, len(merged), "frames", path.stat().st_size // 1024, "KB", imgs[0].size)
