"""Drive a real, embedded Neovim over msgpack-rpc and render its screen grid to HTML."""
import html, os, queue, subprocess, threading, time

import msgpack

import settings as S


class Nv:
    def __init__(self, file=None, cols=S.COLS, rows=S.ROWS):
        env = dict(os.environ, LV_REPO=str(S.REPO), LV_THEME=str(S.theme_dir()))
        if S.LSP:
            env["LV_LSP"] = S.LSP
        args = [S.NVIM, "--embed", "-u", str(S.HERE / "init.lua"), "--noplugin"] + ([file] if file else [])
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=S.PROJECT, env=env)
        self.unp = msgpack.Unpacker(raw=False)
        self.mid = 0
        self.q = queue.Queue()
        self.hl = {}
        self.defaults = (0xcdd6f4, 0x1e1e2e, 0xf38ba8)
        self.cols, self.rows = cols, rows
        self.grid = [[(" ", 0)] * cols for _ in range(rows)]
        self.cursor = (0, 0)
        self.last = time.time()
        threading.Thread(target=self._read, daemon=True).start()
        self.req("nvim_ui_attach", cols, rows, {"ext_linegrid": True, "rgb": True})

    # -- rpc -------------------------------------------------------------------
    def _read(self):
        while True:
            d = self.p.stdout.read1(65536)
            if not d:
                break
            self.unp.feed(d)
            for m in self.unp:
                if m[0] == 2 and m[1] == "redraw":
                    self._redraw(m[2])
                elif m[0] == 1:
                    self.q.put(m)
                elif m[0] == 0:                       # a request from nvim: answer nil
                    self.p.stdin.write(msgpack.packb([1, m[1], None, None]))
                    self.p.stdin.flush()

    def _redraw(self, evs):
        self.last = time.time()
        for ev in evs:
            name = ev[0]
            for a in ev[1:]:
                if name == "default_colors_set":
                    self.defaults = (a[0], a[1], a[2])
                elif name == "hl_attr_define":
                    self.hl[a[0]] = a[1]
                elif name == "grid_resize":
                    self.cols, self.rows = a[1], a[2]
                    self.grid = [[(" ", 0)] * a[1] for _ in range(a[2])]
                elif name == "grid_clear":
                    self.grid = [[(" ", 0)] * self.cols for _ in range(self.rows)]
                elif name == "grid_line":
                    row, col, cells, hl = a[1], a[2], a[3], 0
                    for c in cells:
                        if len(c) > 1:
                            hl = c[1]
                        for _ in range(c[2] if len(c) > 2 else 1):
                            if col < self.cols:
                                self.grid[row][col] = (c[0], hl)
                            col += 1
                elif name == "grid_scroll":
                    top, bot, left, right, rows = a[1], a[2], a[3], a[4], a[5]
                    rng = range(top, bot) if rows > 0 else range(bot - 1, top - 1, -1)
                    for r in rng:
                        s = r + rows
                        if top <= s < bot:
                            self.grid[r][left:right] = self.grid[s][left:right]
                elif name == "grid_cursor_goto":
                    self.cursor = (a[1], a[2])

    def req(self, method, *params, timeout=30):
        self.mid += 1
        self.p.stdin.write(msgpack.packb([0, self.mid, method, list(params)]))
        self.p.stdin.flush()
        while True:
            m = self.q.get(timeout=timeout)
            if m[1] == self.mid:
                if m[2]:
                    raise RuntimeError(m[2])
                return m[3]

    def keys(self, s):
        self.req("nvim_input", s)

    def cmd(self, c):
        self.req("nvim_command", c)

    def lua(self, code):
        return self.req("nvim_exec_lua", code, [])

    def settle(self, quiet=0.7, maxw=20):
        """Wait until the screen has been still for `quiet` seconds."""
        t0 = time.time()
        while time.time() - t0 < maxw:
            time.sleep(0.1)
            if time.time() - self.last > quiet:
                return

    def wait_lsp(self, secs=40):
        t0 = time.time()
        while time.time() - t0 < secs:
            if self.lua("return #vim.lsp.get_clients({bufnr=0})") > 0:
                return
            time.sleep(0.5)

    def close(self):
        try:
            self.cmd("qa!")
        except Exception:
            pass
        self.p.kill()

    # -- rendering -------------------------------------------------------------
    def html(self, font=S.FONT_PX):
        fg0, bg0, _ = self.defaults

        def col(v):
            return "#%06x" % v

        def style(hl):
            a = self.hl.get(hl, {})
            fg, bg = a.get("foreground", fg0), a.get("background", bg0)
            if a.get("reverse"):
                fg, bg = bg, fg
            s = f"color:{col(fg)};background:{col(bg)};"
            if a.get("bold"):
                s += "font-weight:700;"
            if a.get("italic"):
                s += "font-style:italic;"
            if a.get("underline") or a.get("undercurl"):
                s += "text-decoration:underline;"
            return s

        rows = []
        for r, line in enumerate(self.grid):
            out, cur, buf, st = [], None, "", ""
            for c, (t, hl) in enumerate(line):
                key = ("cursor", hl) if (r, c) == self.cursor else hl
                if key != cur:
                    if buf:
                        out.append(f'<span style="{st}">{html.escape(buf)}</span>')
                    cur, buf = key, ""
                    st = f"color:{col(bg0)};background:{col(fg0)};" if isinstance(key, tuple) else style(key)
                buf += t or " "
            if buf:
                out.append(f'<span style="{st}">{html.escape(buf)}</span>')
            rows.append("".join(out))
        return (
            f'<!doctype html><meta charset="utf-8"><body style="margin:0;background:{col(bg0)}">'
            f'<pre style="margin:0;padding:{S.PAD - 2}px {S.PAD}px;'
            f'font:{font}px/{S.LINE_HEIGHT} {S.FONT_FAMILY};'
            f'background:{col(bg0)};color:{col(fg0)};width:max-content">' + "\n".join(rows) + "</pre>"
        )

    def window_size(self, font=S.FONT_PX, margin=S.GIF_MARGIN):
        return (
            int(self.cols * font * S.CELL_W) + margin,
            int(self.rows * font * S.LINE_HEIGHT) + margin,
        )


def chrome_shot(html_text, png_path, size, scale, workdir):
    """Render one HTML string to a PNG with headless Chrome."""
    name = os.path.basename(str(png_path))
    hp = os.path.join(workdir, name + ".html")
    ud = os.path.join(workdir, "ud-" + name)
    with open(hp, "w", encoding="utf-8") as f:
        f.write(html_text)
    subprocess.run(
        [S.CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--user-data-dir={ud}",
         f"--window-size={size[0]},{size[1]}", f"--force-device-scale-factor={scale}",
         f"--screenshot={png_path}", "file:///" + hp.replace("\\", "/")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
