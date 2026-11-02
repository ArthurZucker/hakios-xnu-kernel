#!/usr/bin/env python3
"""Make a captured boot watchable: epd.py leaves you a pile of PNGs, this turns it into something you can look at.

`epd.py --burst DIR` models the panel and writes one frame per repaint; a directory of fNN.png is honest but not
viewable. This reads a capture back three ways:

  --frames   the capture as an animated GIF, so the boot plays
  --compare  the last frame with real content, driven through every waveform in epd.MODES, side by side, so the
             cost of each mode (settle frames, residue) is visible in one picture
  --contact  up to 12 evenly-spaced frames of the boot on one sheet, so a whole boot fits on one image

Labels are drawn with PIL's built-in bitmap font and fall back to 5x7 blocks: the frames are panel images and the
host may have no TTF at all. The panel numbers are not defined here -- they are read from epd.MODES.

usage:
  epd_view.py --frames /tmp/b651/bframes       # -> /tmp/b651/bframes/boot.gif
  epd_view.py --compare /tmp/b651/bframes      # -> /tmp/b651/bframes/compare.png
  epd_view.py --contact /tmp/b651/bframes      # -> /tmp/b651/bframes/contact.png
  epd_view.py --frames DIR --fps 4 --out /tmp/b.gif

Requires: Pillow. Nothing else.
"""
import argparse
import glob
import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("epd_view.py: needs Pillow: python3 -m pip install pillow")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from epd import MODES, settle

GAP = 12
BAR = 30                                   # label strip under a thumbnail
TEXT = 20
BLOCK = 3                                  # 5x7 glyph pixel size
GIF_MAX = 900                              # long side of the animation, so it is small enough to send


def _font():
    """PIL's built-in bitmap font at a size you can read, or None if this build has none."""
    try:
        return ImageFont.load_default(size=TEXT)    # Pillow >= 10.1 can scale the built-in font
    except TypeError:
        try:
            return ImageFont.load_default()
        except Exception:
            return None


FONT = _font()

# 5x7 glyphs, only for Pillow builds without a built-in font, and only the characters the labels use.
_G = {
    "A": "01110/10001/10001/11111/10001/10001/10001",
    "B": "11110/10001/10001/11110/10001/10001/11110",
    "C": "01110/10001/10000/10000/10000/10001/01110",
    "D": "11110/10001/10001/10001/10001/10001/11110",
    "E": "11111/10000/10000/11110/10000/10000/11111",
    "F": "11111/10000/10000/11110/10000/10000/10000",
    "G": "01110/10001/10000/10111/10001/10001/01111",
    "H": "10001/10001/10001/11111/10001/10001/10001",
    "I": "01110/00100/00100/00100/00100/00100/01110",
    "J": "00111/00010/00010/00010/00010/10010/01100",
    "K": "10001/10010/10100/11000/10100/10010/10001",
    "L": "10000/10000/10000/10000/10000/10000/11111",
    "M": "10001/11011/10101/10101/10001/10001/10001",
    "N": "10001/11001/10101/10011/10001/10001/10001",
    "O": "01110/10001/10001/10001/10001/10001/01110",
    "P": "11110/10001/10001/11110/10000/10000/10000",
    "Q": "01110/10001/10001/10001/10101/10010/01101",
    "R": "11110/10001/10001/11110/10100/10010/10001",
    "S": "01111/10000/10000/01110/00001/00001/11110",
    "T": "11111/00100/00100/00100/00100/00100/00100",
    "U": "10001/10001/10001/10001/10001/10001/01110",
    "V": "10001/10001/10001/10001/10001/01010/00100",
    "W": "10001/10001/10001/10101/10101/11011/10001",
    "X": "10001/10001/01010/00100/01010/10001/10001",
    "Y": "10001/10001/01010/00100/00100/00100/00100",
    "Z": "11111/00001/00010/00100/01000/10000/11111",
    "0": "01110/10011/10101/10101/10101/11001/01110",
    "1": "00100/01100/00100/00100/00100/00100/01110",
    "2": "01110/10001/00001/00010/00100/01000/11111",
    "3": "11111/00010/00100/00010/00001/10001/01110",
    "4": "00010/00110/01010/10010/11111/00010/00010",
    "5": "11111/10000/11110/00001/00001/10001/01110",
    "6": "00110/01000/10000/11110/10001/10001/01110",
    "7": "11111/00001/00010/00100/01000/01000/01000",
    "8": "01110/10001/10001/01110/10001/10001/01110",
    "9": "01110/10001/10001/01111/00001/00010/01100",
    " ": "00000/00000/00000/00000/00000/00000/00000",
    ".": "00000/00000/00000/00000/00000/01100/01100",
    "%": "11001/11010/00010/00100/01000/01011/10011",
    ":": "00000/01100/01100/00000/01100/01100/00000",
    "/": "00001/00010/00010/00100/01000/01000/10000",
    "-": "00000/00000/00000/11111/00000/00000/00000",
    "#": "01010/01010/11111/01010/11111/01010/01010",
}
GLYPHS = {k: v.split("/") for k, v in _G.items()}


def put(d, xy, s, fill=(255, 255, 255)):
    """One label, font file never touched."""
    if FONT is not None:
        d.text(xy, s, fill=fill, font=FONT, stroke_width=1, stroke_fill=fill)   # thin at e-ink greys
        return
    x, y = xy
    for ch in s.upper():
        for r, row in enumerate(GLYPHS.get(ch, ())):
            for c, bit in enumerate(row):
                if bit == "1":
                    box = (x + c * BLOCK, y + r * BLOCK, x + (c + 1) * BLOCK - 1, y + (r + 1) * BLOCK - 1)
                    d.rectangle(box, fill=fill)
        x += 6 * BLOCK


def twidth(s):
    return int(FONT.getlength(s)) if FONT is not None else 6 * BLOCK * len(s)


def frames_in(d):
    paths = sorted(glob.glob(os.path.join(d, "f*.png")))
    if not paths:
        sys.exit(f"epd_view.py: no f*.png in {d} (make one: epd.py --burst {d})")
    return paths


def fit(im, h):
    return im.resize((max(1, round(im.width * h / im.height)), h), Image.Resampling.LANCZOS)


def ink(im):
    """Percent of the panel that is not its background colour.

    Not "not white": XNU's console is white on black, so a blank black panel is 100% non-white and 0% ink. The modal
    colour is the panel, whatever colour it is.
    """
    im = im.convert("L")
    cols = im.getcolors(256) or [(1, 0)]
    bg = max(cols)[1]
    return 100.0 * sum(c for c, v in cols if v != bg) / (im.width * im.height)


def content(paths):
    """The last frame that drew something, not the blank panel the loader leaves behind."""
    best, besti = -1.0, 0
    for i, p in enumerate(paths):
        v = ink(Image.open(p))
        if v >= best:                          # ties go to the later frame
            best, besti = v, i
    return besti


def even(items, n):
    if len(items) <= n:
        return items
    return [items[round(i * (len(items) - 1) / (n - 1))] for i in range(n)]


def gif(paths, out, fps):
    """The capture as an animation. Frames change size mid-boot (UEFI is 800x600, the loader then 1125x2436, and XNU
    draws there), so all of them are scaled to one height and centred on one grey canvas: the size change is real,
    the jump in the GIF is not."""
    shown = [Image.open(p).convert("RGB") for p in paths]
    h = min(round(GIF_MAX * i.height / max(i.width, i.height)) for i in shown)
    ims = [fit(i, h) for i in shown]
    w = max(i.width for i in ims)
    out_frames = []
    for im in ims:
        c = Image.new("RGB", (w, h), (128, 128, 128))
        c.paste(im, ((w - im.width) // 2, 0))
        out_frames.append(c.convert("P", palette=Image.Palette.ADAPTIVE, colors=256))
    out_frames[0].save(out, "GIF", save_all=True, append_images=out_frames[1:],
                       duration=round(1000.0 / fps), loop=0)
    return w, h


def compare(paths, out):
    """The whole capture replayed under each waveform, so the four results come from the same input frames.

    A single frame would give the same picture four times: with no previous panel, settle() has nothing to ghost.
    """
    i = content(paths)
    h = 700
    cells = []
    for mode in sorted(MODES):
        shown, driven = None, None
        for k, p in enumerate(paths[:i + 1]):
            frame = Image.open(p).convert("RGB")
            shown = settle(frame, driven, mode, k)      # residue comes from the frame driven, not from how it settled
            driven = frame
        frames_n, _, residue = MODES[mode]
        label = f"{mode.upper()}  {frames_n} FRAMES  {residue * 100:.0f}% RESIDUE"
        cells.append((label, fit(shown, h)))
    cw = max(max(c[1].width, twidth(c[0]) + 2 * GAP) for c in cells)
    ch = h + BAR
    top = BAR
    sheet = Image.new("RGB", (GAP + len(cells) * (cw + GAP), top + ch + GAP), (128, 128, 128))
    d = ImageDraw.Draw(sheet)
    title = f"LAST CONTENT FRAME {os.path.basename(paths[i])[:-4].upper()}  /  {len(paths)} FRAMES  /  ONE BOOT, FOUR WAVEFORMS"
    put(d, (GAP, 4), title, fill=(0, 0, 0))
    for k, (label, im) in enumerate(cells):
        x = GAP + k * (cw + GAP) + (cw - im.width) // 2
        y = top
        sheet.paste(im, (x, y))
        d.rectangle((x, y + h, x + im.width - 1, y + h + BAR - 1), fill=(0, 0, 0))
        put(d, (x + 6, y + h + 4), label)
    sheet.save(out, "PNG")
    return sheet.size, os.path.basename(paths[i]), len(paths)


def contact(paths, out):
    """Up to 12 frames of the boot on one sheet, timestamped from the first capture."""
    pick = even(paths, 12)
    h = 380
    t0 = os.path.getmtime(pick[0])
    cells = [(f"{os.path.basename(p)[:-4].upper()}  {os.path.getmtime(p) - t0:.1f}S", fit(Image.open(p).convert("RGB"), h))
             for p in pick]
    cw = max(max(c[1].width, twidth(c[0]) + 2 * GAP) for c in cells)
    cols = min(4, len(cells))
    rows = math.ceil(len(cells) / cols)
    sheet = Image.new("RGB", (GAP + cols * (cw + GAP), GAP + rows * (h + BAR + GAP)), (128, 128, 128))
    d = ImageDraw.Draw(sheet)
    for k, (label, im) in enumerate(cells):
        x = GAP + (k % cols) * (cw + GAP) + (cw - im.width) // 2
        y = GAP + (k // cols) * (h + BAR + GAP)
        sheet.paste(im, (x, y))
        d.rectangle((x, y + h, x + im.width - 1, y + h + BAR - 1), fill=(0, 0, 0))
        put(d, (x + 6, y + h + 4), label)
    sheet.save(out, "PNG")
    return sheet.size, len(pick), len(paths), cols, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--frames", metavar="DIR", help="write DIR/boot.gif, the capture as an animation")
    g.add_argument("--compare", metavar="DIR", help="write DIR/compare.png, the last content frame in every waveform")
    g.add_argument("--contact", metavar="DIR", help="write DIR/contact.png, up to 12 frames of the boot")
    ap.add_argument("--out", metavar="FILE", help="default: DIR/boot.gif, DIR/compare.png or DIR/contact.png")
    ap.add_argument("--fps", type=float, default=2.0, help="rate the capture was taken at (GIF delay), default 2")
    a = ap.parse_args()

    d = a.frames or a.compare or a.contact
    paths = frames_in(d)
    os.makedirs(os.path.dirname(os.path.abspath(a.out or d)), exist_ok=True)

    if a.frames:
        out = a.out or os.path.join(d, "boot.gif")
        w, h = gif(paths, out, a.fps)
        what = f"{len(paths)} frames at {a.fps:g} fps, {w}x{h}"
    elif a.compare:
        out = a.out or os.path.join(d, "compare.png")
        (w, h), last, n = compare(paths, out)
        what = f"{len(MODES)} waveforms of {last} from {n} captured frames, {w}x{h}"
    else:
        out = a.out or os.path.join(d, "contact.png")
        (w, h), shown, n, cols, rows = contact(paths, out)
        what = f"{shown} of {n} frames, {cols}x{rows} grid, {w}x{h}"
    print(f"{out}  {what}")


if __name__ == "__main__":
    sys.exit(main())
