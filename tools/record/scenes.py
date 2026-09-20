"""The recordings.  Run `python tools/record/scenes.py --list`, or see README.md."""
import argparse, shutil, sys

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))

from PIL import Image

import settings as S
from recorder import Rec

GIFS = {}      # output name -> function
STILLS = {}    # output name -> function


def gif(fn):
    GIFS[fn.__name__] = fn
    return fn


def still(fn):
    STILLS[fn.__name__] = fn
    return fn


# -- GIFs for the Demos page (installed into assets/videos/) -------------------
@gif
def Format():
    r = Rec("Format", "rtl/fmt_demo.sv")
    r.hold()
    r.cmd("Format", run_ms=2500)
    r.finish()


@gif
def AutoInst():
    r = Rec("AutoInst", "rtl/inst_demo.sv")
    r.cursor(7, 12)
    r.hold()
    r.code_action("AutoInst")
    r.hold()
    r.finish()


@gif
def AutoWire():
    r = Rec("AutoWire", "rtl/wire_demo.sv")
    r.cursor(8, 6)
    r.hold()
    r.code_action("AutoWire")
    r.keys("y", 3000, 1.5)                       # apply the preview
    r.finish()


@gif
def AutoArg():
    r = Rec("AutoArg", "rtl/arg_demo.sv")
    r.cursor(1, 8)
    r.hold()
    r.code_action("AutoArg")
    r.hold()
    r.finish()


@gif
def AutoComplete():
    r = Rec("AutoComplete", "rtl/comp_demo.sv")
    r.cursor(9, 1)
    r.hold(1400)
    r.keys("o", 300)
    r.type("    .", ms=300, settle=1.2)
    r.keys("<C-x><C-o>", 1600, 2.5)              # omnifunc: the ports of alu
    r.keys("<C-n>", 1000, 0.5)
    r.keys("<C-y>", 800, 0.6)
    r.type("i_x", ms=150, settle=0.2)            # the snippet leaves the name selected
    r.keys("<Esc>", 300, 0.3)
    r.keys("A,", 300, 0.3)
    r.keys("<CR>", 300, 0.4)
    r.type(".", ms=300, settle=1.2)
    r.keys("<C-x><C-o>", 1800, 2.5)              # only the ports not yet connected
    r.keys("<C-n>", 1000, 0.5)
    r.keys("<C-y>", 800, 0.6)
    r.type("i_y", ms=150, settle=0.2)
    r.keys("<Esc>", 300, 0.3)
    r.hold(2400)
    r.finish()


@gif
def RtlTree():
    # Enter on a tree entry is not recorded: on Windows it opens an empty buffer (the tree
    # paths carry a leading slash, "/C:/...").
    r = Rec("RtlTree", "rtl/soc_top.sv")
    r.hold(1000)
    r.cmd("RtlTree", run_ms=2200, settle=2.5)
    r.keys("<C-w>h", 500, 0.4)
    for _ in range(5):
        r.keys("j", 400, 0.3)
    r.keys("j", 900, 0.3)
    r.hold(500)
    for _ in range(2):
        r.keys("k", 500, 0.3)
    r.hold(2500)
    r.finish()


@gif
def Folding():
    r = Rec("Folding", "rtl/fold_demo.sv")
    r.hold(1200)
    for line in (10, 13, 16):
        r.cursor(line, 6)
        r.keys("zc", 1400, 0.6)
    r.keys("zR", 1600, 0.6)
    r.keys("zM", 1800, 0.6)
    r.hold(1200)
    r.finish()


@gif
def GoToDef():
    r = Rec("GoToDef", "rtl/soc_top.sv")
    r.cursor(15, 5)
    r.hold(1400)
    r.keys("gd", 2000, 1.5)
    r.cursor(16, 5)
    r.hold(900)
    r.keys("gd", 2000, 1.5)
    r.keys("<C-o>", 1200, 0.8)
    r.keys("<C-o>", 1800, 0.8)
    r.finish()


@gif
def hover():
    r = Rec("hover", "rtl/soc_top.sv")
    r.cursor(15, 5)
    r.hold(1200)
    r.keys("K", 2800, 1.5)
    r.keys("<Esc>", 300, 0.3)
    r.keys("<Esc>", 300, 0.3)
    r.cursor(6, 22)
    r.hold(600)
    r.keys("K", 2800, 1.5)
    r.finish()


@gif
def sig_help():
    r = Rec("sig_help", "rtl/sig_demo.sv")
    r.cursor(11, 1)
    r.hold(1200)
    r.keys("S", 300)
    r.type("add_number(", ms=110, settle=0.4)
    r.keys("<C-s>", 1800, 1.0)
    r.type("x, ", ms=200, settle=0.4)
    r.keys("<C-s>", 1800, 1.0)
    r.type("y, ", ms=200, settle=0.4)
    r.keys("<C-s>", 2200, 1.0)
    r.finish()


@gif
def get_reference():
    r = Rec("get_reference", "rtl/soc_top.sv")
    r.cursor(9, 20)
    r.hold(1200)
    r.keys("gr", 2400, 2.0)
    r.keys("j", 700, 0.4)
    r.keys("j", 700, 0.4)
    r.keys("<CR>", 2200, 1.0)
    r.finish()


@gif
def rename():
    r = Rec("rename", "rtl/soc_top.sv")
    r.cursor(7, 20)
    r.hold(1200)
    r.keys("grn", 1200, 1.0)
    r.keys("<C-u>", 300, 0.2)
    r.type("cpu_data_out", ms=70, settle=0.1)
    r.hold(500)
    r.keys("<CR>", 3000, 2.0)
    r.finish()


@gif
def workspace_symbols():
    r = Rec("workspace_symbols", "rtl/soc_top.sv")
    r.hold(1000)
    r.cmd("Symbols _", run_ms=2600, settle=2.5)
    for _ in range(3):
        r.keys("j", 500, 0.3)
    r.keys("<CR>", 2400, 1.2)
    r.finish()


@gif
def InterfaceConnect():
    r = Rec("InterfaceConnect", "rtl/soc_top.sv")
    r.hold(1000)
    r.cmd("Interface u_mem u_cpu", run_ms=2600, settle=2.5)
    r.keys("C", 1200, 1.0)
    r.type("5", ms=700, settle=0.3)
    r.keys("<CR>", 1000, 0.8)
    r.type("12", ms=700, settle=0.3)
    r.keys("<CR>", 1000, 0.8)
    r.type("irq", ms=150, settle=0.3)
    r.hold(600)
    r.nv.keys("<CR>")
    r.nv.settle(3, 10)                           # the window's own refresh fails here; skip it
    r.nv.keys("q")
    r.nv.settle(1, 5)
    r.cmd("Interface u_mem u_cpu", run_ms=3000, settle=3.0)   # the connection, in the table
    r.keys("q", 300, 0.5)
    r.nv.keys(":<Esc>")                          # clear the old command line
    r.nv.settle(0.5, 3)
    r.cursor(1, 1)
    r.hold(3200)                                 # the new wire and .i_irq (irq)
    r.cursor(38, 1)
    r.keys("zt", 3800, 0.6)                      # .o_irq (irq)
    r.finish()


# -- Still images ---------------------------------------------------------------
@still
def lint_diagnostics():
    r = Rec("lint_diagnostics", "rtl/m_lint_demo.sv", cols=110, rows=22)
    r.nv.lua("vim.diagnostic.enable(true); vim.diagnostic.config({virtual_text=true, signs=true, underline=true})")
    r.nv.settle(3, 10)
    r.shot()


@still
def inlay_hint():
    r = Rec("inlay_hint", "rtl/soc_top.sv", rows=22)
    r.cursor(15, 5)
    r.nv.settle(1.5, 5)
    r.shot()


# Documentation screenshots (installed into docs/public/screenshots/ as WebP).
@still
def rtl_tree_neovim():
    r = Rec("rtl-tree-neovim", "rtl/soc_top.sv", cols=110, rows=22)
    r.nv.keys(":RtlTree<CR>")
    r.nv.settle(2.5, 15)
    r.shot()


@still
def interface_neovim():
    r = Rec("interface-neovim", "rtl/soc_top.sv", rows=30)
    r.nv.keys(":Interface u_cpu u_bus<CR>")
    r.nv.settle(2.5, 15)
    r.shot()


@still
def interface_one_neovim():
    r = Rec("interface-one-neovim", "rtl/soc_top.sv", rows=30)
    r.nv.keys(":Interface u_cpu<CR>")
    r.nv.settle(2.5, 15)
    r.shot()


@still
def connect_port_neovim():
    r = Rec("connect-port-neovim", "rtl/soc_top.sv", rows=30)
    r.nv.keys(":Connect mem_ctrl cpu_core<CR>")
    r.nv.settle(3, 15)
    r.nv.keys("<CR>")
    r.nv.settle(2, 10)
    r.shot()


@still
def connect_preview_neovim():
    r = Rec("connect-preview-neovim", "rtl/soc_top.sv", rows=30)
    nv = r.nv
    nv.keys(":Connect mem_ctrl cpu_core<CR>")
    nv.settle(3, 15)
    for keys in ("<CR>", "o_irq", "<CR>", "<CR>", "i_irq", "<CR>", "irq", "<CR>"):
        nv.keys(keys)
        nv.settle(1.5 if keys != "<CR>" else 2, 10)
    nv.settle(3, 10)
    r.shot()


DOC_SHOTS = ("rtl-tree-neovim", "interface-neovim", "interface-one-neovim",
             "connect-port-neovim", "connect-preview-neovim")


def install():
    videos = S.REPO / "assets" / "videos"
    shots = S.REPO / "docs" / "public" / "screenshots"
    shots.mkdir(parents=True, exist_ok=True)
    for f in sorted(S.OUT.glob("*.gif")):
        shutil.copy(f, videos / f.name)
        print("->", videos / f.name)
    for f in sorted(S.OUT.glob("*.png")):
        if f.name.endswith("-last.png"):
            continue
        stem = f.stem
        if stem in DOC_SHOTS:
            Image.open(f).convert("RGB").save(shots / (stem + ".webp"), quality=90, method=6)
            print("->", shots / (stem + ".webp"))
        elif stem in ("lint_diagnostics", "inlay_hint"):
            shutil.copy(f, videos / f.name)
            print("->", videos / f.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("names", nargs="*", help="scene names; none with --all")
    ap.add_argument("--all", action="store_true", help="every scene")
    ap.add_argument("--list", action="store_true", help="list the scenes")
    ap.add_argument("--install", action="store_true", help="copy out/ into assets/videos and docs/public/screenshots")
    a = ap.parse_args()
    scenes = {**GIFS, **STILLS}
    # the still scenes are written with underscores; their files use dashes
    if a.list:
        print("GIFs:  ", " ".join(GIFS))
        print("stills:", " ".join(STILLS))
        return
    names = list(scenes) if a.all else a.names
    if not names and not a.install:
        ap.error("name a scene, or use --all, --list, --install")
    for n in names:
        if n not in scenes:
            ap.error(f"unknown scene {n!r}; try --list")
        scenes[n]()
    if a.install:
        install()


if __name__ == "__main__":
    main()
