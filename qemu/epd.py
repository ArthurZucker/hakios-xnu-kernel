#!/usr/bin/env python3
"""The HiBreak Pro's e-ink panel, as a model you can watch.

Reads the framebuffer of a running run-b651.sh QEMU over its monitor socket, and shows what an
electrophoretic panel would actually have done with the frames it was given: it does not update
instantly, it ghosts, partial updates leave residue, and a full refresh flashes black-white-black.

That is the point: a normal framebuffer hides every problem an e-ink driver has. This one exposes them.

usage:
  epd.py                     # follow the running QEMU, print what it would look like
  epd.py --once out.png      # one frame, as a PNG, and exit
  epd.py --fps 2             # how fast the panel repaints (default 2: e-ink, not 60)
  epd.py --mode gc16         # waveform: gc16 (full, flashy), du (fast, ghosts), a2 (fastest, worst)

QEMU_INSTANCE picks which copy of QEMU to read, as in qemu/stop.sh.

Requires: Pillow (pip install pillow). Nothing else.
"""
import argparse
import json
import os
import socket
import sys
import time

try:
    from PIL import Image, ImageChops, ImageDraw, ImageStat
except ImportError:
    sys.exit("epd.py: needs Pillow: python3 -m pip install pillow")

# The panel. 1125x2436 is the mode the loader sets and XNU draws at (`display 1125 x 2436`, `MiniIOMobileFramebuffer:
# display 1125x2436`), so it is the panel geometry that matters here. The unit's Android `screencap` comes out at
# 824x1648, which is a different logical mode -- using it here cropped the right edge off every console line.
PANEL_W, PANEL_H = 1125, 2436

# Waveform families an e-ink panel supports. Each is (frames to settle, whether it flashes, ghost residue
# after the update). The numbers are a plausible model of the named modes, not measurements.
MODES = {
    "gc16": (24, True, 0.00),         # full grey-level clear: slow, flashes, leaves nothing
    "gl16": (16, False, 0.06),        # grey-level, no flash: what GL16 means in eink_glx16_disable
    "du":   (6,  False, 0.22),        # direct update: fast, leaves a shadow of the old image
    "a2":   (2,  False, 0.45),        # animation: fastest and dirtiest
}

# What the panel can actually hold. Electrophoresis is not 8-bit: the controller drives a fixed number of grey levels
# per pixel, and even the extremes are not the framebuffer's black and white -- undriven ink still reflects light, and
# the white state is pigment, not a backlight. All three values are ASSUMED, not measured: the real bit depth and the
# ink/paper luminance come from the panel's waveform LUTs and a photometer, and this host has neither. Change
# GREY_LEVELS when the driver reports the bit depth; change the two colours when the panel is measured.
GREY_LEVELS = 16          # ASSUMED: 4-bit greyscale, the common e-ink depth
INK_BLACK = 0x22          # ASSUMED: darkest state, not 0x00
PAPER_WHITE = 0xDD        # ASSUMED: lightest state, not 0xFF

# How many levels each waveform can address per pixel. a2 and du are the black/white modes -- a2 is documented as
# "fast black/white animation" (mxc_epdc_fb_test.c:68) -- so quantising them to the full grey ramp would show a
# picture the panel cannot make. devices/b651/WAVEFORMS.md makes this argument; 16 for the grey modes is ASSUMED.
MODE_LEVELS = {"gc16": GREY_LEVELS, "gl16": GREY_LEVELS, "du": 2, "a2": 2}

# Every number above is ASSUMED. They are collected here so a sweep can change them without editing this file:
#   epd.py --calibrate        print this table, then render a strip per mode to compare against the phone
#   epd.py --set du.frames=8  change one and show what it looks like (repeatable; --save writes it out)
# A calibration written by --save lands in qemu/panel.json and is loaded below, if it exists. That file is the measured
# truth; these literals are only what the panel does until someone measures it. devices/b651/CALIBRATE.md.
KNOBS = {
    "grey.levels":   GREY_LEVELS,   # how many greys the gc16/gl16 waveforms can address
    "grey.black":    INK_BLACK,     # darkest the ink gets, of 255
    "grey.white":    PAPER_WHITE,   # lightest, of 255
    "gc16.frames":   MODES["gc16"][0],
    "gc16.flash":    MODES["gc16"][1],
    "gc16.residue":  MODES["gc16"][2],
    "gl16.frames":   MODES["gl16"][0],
    "gl16.flash":    MODES["gl16"][1],
    "gl16.residue":  MODES["gl16"][2],
    "du.frames":     MODES["du"][0],
    "du.flash":      MODES["du"][1],
    "du.residue":    MODES["du"][2],
    "du.levels":     MODE_LEVELS["du"],
    "a2.frames":     MODES["a2"][0],
    "a2.flash":      MODES["a2"][1],
    "a2.residue":    MODES["a2"][2],
    "a2.levels":     MODE_LEVELS["a2"],
    "panel.fps":     2.0,           # how often the model repaints, as the controller would be told
}

CALIBRATION = os.path.join(os.path.dirname(os.path.abspath(__file__)), "panel.json")


def apply(knobs):
    """Fold `knobs` (any subset of KNOBS) into the module's own tables, in place.

    The tables are what everything here already reads, so a calibration changes the model rather than a parallel copy
    of it: settle() unpacks MODES[mode] as (frames, flash, residue), and panel() takes the level count.
    """
    global GREY_LEVELS, INK_BLACK, PAPER_WHITE
    for mode in MODES:
        MODES[mode] = (int(knobs[f"{mode}.frames"]), bool(knobs[f"{mode}.flash"]),
                       float(knobs[f"{mode}.residue"]))
        MODE_LEVELS[mode] = int(knobs[f"{mode}.levels"]) if f"{mode}.levels" in knobs else 2
    GREY_LEVELS = int(knobs["grey.levels"])
    INK_BLACK = int(knobs["grey.black"])
    PAPER_WHITE = int(knobs["grey.white"])
    MODE_LEVELS["gc16"] = MODE_LEVELS["gl16"] = GREY_LEVELS
    return knobs


def load(path=CALIBRATION):
    """Take a written calibration if there is one. Absent is not an error: the defaults are the fallback."""
    if not os.path.exists(path):
        return None
    try:
        with open(path) as f:
            saved = json.load(f)
    except (OSError, ValueError) as e:
        print(f"epd.py: {path} is not readable as JSON ({e}); using the built-in numbers", file=sys.stderr)
        return None
    knobs = dict(KNOBS)
    knobs.update({k: v for k, v in saved.items() if k in KNOBS})     # unknown keys are a newer/older file: ignore
    apply(knobs)
    KNOBS.update(knobs)
    return path


load()          # a written calibration wins over the literals above, for every importer of this module


class Qemu:
    def __init__(self, instance=0):
        self.port = 4444 + 10 * instance
        try:
            self.s = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        except OSError as e:
            sys.exit(f"epd.py: no QEMU monitor on 127.0.0.1:{self.port} ({e}). Start qemu/run-b651.sh first.")
        self.s.settimeout(5)
        self.s.sendall(b"\n")
        time.sleep(0.2)
        self._drain()

    def _drain(self):
        out = b""
        while True:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
            if b"(qemu)" in out:
                break
        return out

    def ppm(self, path):
        self.s.sendall(f"screendump {path}\n".encode())
        time.sleep(0.35)
        self._drain()
        try:
            raw = open(path, "rb").read()
            os.unlink(path)
        except OSError:
            return None
        if not raw.startswith(b"P6"):
            return None
        # PPM header: magic, width height, maxval -- then binary rows, which QEMU pads to 4 bytes
        fields, i = [], 2
        while len(fields) < 3 and i < len(raw):
            while i < len(raw) and raw[i:i + 1].isspace():
                i += 1
            if raw[i:i + 1] == b"#":
                while i < len(raw) and raw[i:i + 1] != b"\n":
                    i += 1
                continue
            j = i
            while j < len(raw) and not raw[j:j + 1].isspace():
                j += 1
            fields.append(int(raw[i:j]))
            i = j
        w, h = fields[0], fields[1]
        pix = raw[i + 1:]
        stride = len(pix) // h
        rows = b"".join(pix[y * stride:y * stride + w * 3] for y in range(h))
        return Image.frombytes("RGB", (w, h), rows)


def crop(img, w=PANEL_W, h=PANEL_H):
    return img.crop((0, 0, min(w, img.width), min(h, img.height)))


def panel(img, levels=GREY_LEVELS):
    """What the panel can actually show, as opposed to what the framebuffer asked for.

    Two things a real controller does that a framebuffer does not: it snaps every pixel to one of `levels` grey
    levels, and it can only reach INK_BLACK..PAPER_WHITE, never 0..255. Luma first, because an electrophoretic pixel
    has no colour; then the level index; then the ink. `levels` is the waveform's, not the panel's: a2/du address two
    states per pixel and would otherwise be drawn with a grey ramp they cannot make (MODE_LEVELS). This is applied at
    the edge, to the picture, not to the state carried between frames: `old` stays the frame the panel was *driven*
    with (see settle), so ghosting is computed in framebuffer space and does not compound through the narrower panel
    range.
    """
    span = PAPER_WHITE - INK_BLACK
    lut = [INK_BLACK + round(round(v * (levels - 1) / 255.0) * span / (levels - 1))
           for v in range(256)]
    return img.convert("L").point(lut).convert("RGB")


def settle(new, old, mode, history, rect=None):
    """What the panel shows after driving `new` with waveform `mode`.

    Three effects, in the order a real controller applies them:
      1. a full (flashing) mode blanks the panel first, so the whole screen goes black then white
      2. pixels arrive over several frames, so a partial capture sees a mix of old and new
      3. a non-flashing mode leaves residue: the previous image at a fraction of its contrast

    `old` is the frame the panel was last *driven* with, not the result of settling it: residue is ink the controller
    failed to clear, so feeding a settled image back in compounds the dimming every update and the panel fades to
    black. `history` is how many partial updates have gone by since the last full clear; residue grows with it, which
    is why a panel needs a periodic full refresh. A full (flashing) mode resets it; the caller owns the count.

    The residue is per pixel, not a flat wash: a pixel driven a long way (black to white) has more ink to clear and
    ghosts harder than one that barely moved, and a pixel that did not change at all keeps its content with no shadow.
    `ghost` in MODES is the scale factor -- how much of a full swing survives -- so the modes still read as before.

    `rect` (x0, y0, x1, y1) is the damage rect: only those pixels are driven, everything outside keeps the frame the
    panel already had. That is why a partial update is fast -- the panel does not re-settle pixels nobody touched. It
    matters physically too: driving the whole panel for a two-line change is what a controller must never do.
    """
    frames, flashes, ghost = MODES[mode]
    if old is None or old.size != new.size:
        # a new mode is a new panel scan: there is nothing to ghost over, the controller re-drives every pixel
        return panel(new, MODE_LEVELS[mode])
    if flashes:
        # the waveform drives every pixel to black, then to white, then to its target: the picture is clean, so
        # the residue is zero. Model the settled state; a capture taken mid-flash shows a blank screen instead,
        # which is why a phone running a flashing refresh visibly blinks -- that is epd_view.py's flash frames.
        out = new.copy()
    else:
        # ink already on the panel is not fully cleared, so where the new image has no ink the old one still shows
        # through. Take the darker of the two (this is a white-on-black console: darker == more ink), then fade that
        # toward the clean result by the residue fraction -- scaled per pixel by how far that pixel was driven.
        grown = min(0.9, ghost * (1.0 + 0.5 * max(0, history - 1)))
        alpha = ImageChops.difference(new, old).convert("L").point(lambda v: int(grown * v))
        out = Image.composite(ImageChops.darker(new, old), new, alpha)
    if rect is not None and not flashes:
        # outside the rect the panel is not addressed at all: it keeps what it had, and only the rect takes the update
        driven = old.copy()
        driven.paste(out.crop(rect), rect)
        out = driven
    return panel(out, MODE_LEVELS[mode])


def damage(new, old):
    """The rect that changed, or None. This is the number a real controller is given, and the reason a partial
    update is fast: the panel only drives the pixels that moved."""
    diff = ImageChops.difference(new, old).convert("L")
    return diff.point(lambda v: 255 if v > 8 else 0).getbbox()


# Under this share of the panel a change is not worth a full flash: the fast waveforms are used on its rect only.
# Over it, or once a partial streak has gone PAINT_LIMIT updates deep, a full refresh is cheaper than the ghosting.
FAST_AREA = 0.30
PAINT_LIMIT = 12


def auto_mode(rect, history):
    """Which waveform a controller with damage rects would pick. The whole point of `a2`/`du` is that a small
    change costs 2-6 frames on part of the panel instead of 24 on all of it."""
    if rect is None:
        return None                                     # nothing moved: do not update the panel at all
    area = (rect[2] - rect[0]) * (rect[3] - rect[1]) / float(PANEL_W * PANEL_H)
    if area > FAST_AREA or history >= PAINT_LIMIT:
        return "gc16"                                   # full clear: large change, or the ghosting has caught up
    return "a2" if area < 0.05 else "du"


def to_png(img, path):
    img.save(path, "PNG")


def burst(a):
    """Follow a boot and write what the panel showed, frame by frame.

    QEMU's monitor dies with the guest, so this is the mode to use on a boot: a single --once almost always misses
    the seconds between the loader handing over and XNU drawing. Each frame is settled against the previous one with
    the chosen waveform, so the files are what the screen looked like, not what the framebuffer held.
    """
    os.makedirs(a.burst, exist_ok=True)
    q = None
    prev, n, t0 = None, 0, time.time()
    partial = 0                      # partial updates since the last full clear: what residue accumulates on
    while True:
        if q is None:
            try:
                q = Qemu(a.instance)
            except SystemExit:
                return 0                      # the guest has not started the monitor yet, or is gone
        try:
            raw = q.ppm(os.path.join(a.burst, "scratch.ppm"))
        except OSError:
            print(f"{n} frames, {time.time() - t0:.1f}s: the guest ended")
            return 0
        if raw is not None:
            frame = crop(raw)
            if prev is None or prev.size != frame.size:
                rect = (0, 0, frame.width, frame.height)     # a new scan: the whole panel
            else:
                rect = damage(frame, prev)                  # None: nothing moved
            mode = a.mode if a.mode != "auto" else auto_mode(rect, partial)
            if mode is None:
                time.sleep(1.0 / a.fps)       # nothing changed: a controller does not update at all
                continue
            shown = settle(frame, prev, mode, partial, rect if a.mode == "auto" else None)
            partial = 0 if MODES[mode][1] else partial + 1
            n += 1
            path = os.path.join(a.burst, f"f{n:02d}.png")
            to_png(shown, path)
            where = f"rect={rect[0]},{rect[1]}-{rect[2]},{rect[3]}" if rect and a.mode == "auto" else "whole panel"
            print(f"[{time.time() - t0:5.1f}s] f{n:02d} {shown.width}x{shown.height} {mode} {where} "
                  f"{describe(shown)} partial={partial}")
            # residue comes from the frame the panel was driven with, not from how it settled (see settle()),
            # and outside a damage rect it was not driven at all: that region still holds the older frame
            if rect is not None and a.mode == "auto" and not MODES[mode][1] and prev is not None:
                frame = frame.copy()
                frame.paste(prev.crop(rect), rect)
            prev = frame
        time.sleep(1.0 / a.fps)


def describe(img):
    """How much of the panel is not the background colour, and what that background is.

    Counting "not white" is wrong for a text console: XNU draws white on black, so a blank panel and a full one
    both read 100%. The background is the modal colour; ink is everything else.
    """
    cols = img.getcolors(1 << 20) or []
    if not cols:
        return "empty"
    tot = sum(c for c, _ in cols)
    bg_n, bg = max(cols)
    return f"colours={len(cols):3d} bg={bg[0]:3d},{bg[1]:3d},{bg[2]:3d} ink={100.0 * (tot - bg_n) / tot:5.1f}%"


def show_knobs():
    """The table, with the value each knob has right now."""
    saved = os.path.exists(CALIBRATION)
    print(f"# {'knob':16s} {'value':>8s}   what it is")
    print(f"# (numbers from {CALIBRATION if saved else 'the built-in defaults'})")
    for k, v in KNOBS.items():
        print(f"  {k:16s} {v!s:>8s}")


def strip():
    """A test page through the panel model: ramp, text-sized bars, and one flat patch per wave state.

    Rendered at the current knobs so a change is visible: fewer levels show as fewer bands, a narrower INK_BLACK..
    PAPER_WHITE range shows as less contrast at both ends. Returned as an image, not written anywhere.
    """
    w, h = 480, 360
    im = Image.new("RGB", (w, h), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for x in range(w):                                   # a ramp: the panel should break this into GREY_LEVELS bands
        d.line([(x, 0), (x, 60)], fill=(x * 255 // (w - 1),) * 3)
    for i in range(16):                                  # bar widths, what text strokes are made of
        d.rectangle([8, 80 + i * 9, 8 + 3 * (i + 1), 86 + i * 9], fill=(0, 0, 0))
    d.text((160, 80), "HakiOS  e-ink", fill=(0, 0, 0))
    for i, mode in enumerate(("gc16", "gl16", "du", "a2")):   # one settled patch per waveform
        patch = Image.new("RGB", (110, 70), (128, 128, 128))
        out = settle(patch, Image.new("RGB", (110, 70), (0, 0, 0)), mode, 1)
        im.paste(out, (8 + (i % 4) * 116, 260))
        d.text((8 + (i % 4) * 116, 332), f"{mode} {MODES[mode][0]}f", fill=(0, 0, 0))
    return panel(im)


def calibrate(a):
    """--calibrate: the table, then the test page at those numbers. --set changes them first."""
    if a.set:
        knobs = dict(KNOBS)
        for one in a.set:
            k, _, v = one.partition("=")
            if k not in KNOBS:
                sys.exit(f"epd.py: no knob {k!r}. --calibrate prints the list.")
            knobs[k] = (v.lower() in ("1", "true", "yes")) if isinstance(KNOBS[k], bool) else type(KNOBS[k])(v)
        apply(knobs)
        KNOBS.update(knobs)
    show_knobs()
    out = a.calibrate
    if out:
        strip().save(out)
        print(f"\n{out}  the panel model's own test page at those numbers")
    if a.save:
        with open(CALIBRATION, "w") as f:
            json.dump(KNOBS, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"\n{CALIBRATION}  written: every run of epd.py, epd_live.py and epd_view.py uses these from now on")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", type=int, default=int(os.environ.get("QEMU_INSTANCE", 0)))
    ap.add_argument("--fps", type=float, default=2.0)
    ap.add_argument("--mode", default="gc16", choices=sorted(MODES) + ["auto"],
                    help="waveform; 'auto' picks per frame from the damage rect, as a controller does")
    ap.add_argument("--once", metavar="PNG", help="one settled frame, write a PNG, exit")
    ap.add_argument("--burst", metavar="DIR", help="follow a boot frame by frame, writing DIR/fNN.png, and exit "
                                                   "when the guest does. Use this: a boot is over in seconds.")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: forever)")
    ap.add_argument("--calibrate", metavar="PNG", nargs="?", const="", default=None,
                    help="print every calibration knob, and render the test page to PNG if given")
    ap.add_argument("--set", action="append", metavar="KNOB=VALUE",
                    help="change a knob for this run, e.g. --set du.residue=0.35 (repeatable)")
    ap.add_argument("--save", action="store_true", help="with --calibrate: write the knobs to qemu/panel.json")
    a = ap.parse_args()

    if a.calibrate is not None:
        return calibrate(a)
    if a.set:
        knobs = dict(KNOBS)
        for one in a.set:
            k, _, v = one.partition("=")
            knobs[k] = type(KNOBS[k])(v) if k in KNOBS else sys.exit(f"epd.py: no knob {k!r}")
        apply(knobs)


    if a.burst:
        return burst(a)

    q = Qemu(a.instance)
    ppm = os.path.join(os.path.dirname(os.path.abspath(__file__)), "b651-screen.ppm")
    shown, driven, t0 = None, None, time.time()     # driven: the frame the panel was last given, the residue source
    n = 0
    while True:
        raw = q.ppm(ppm)
        if raw is not None:
            frame = crop(raw)
            shown = settle(frame, driven, a.mode, n)
            driven = frame
            n += 1
            if a.once:
                to_png(shown, a.once)
                print(f"{a.once}  {shown.width}x{shown.height}  mode={a.mode}  frames={n}")
                return 0
            # the terminal cannot show a picture; say what changed instead of pretending
            if n > 1:
                diff = ImageChops.difference(shown, prev).convert("L")
                st = ImageStat.Stat(diff)
                print(f"[{n:4d}] mean|delta|={st.mean[0]:6.2f}  mode={a.mode}")
            prev = shown.copy()
        if a.seconds and time.time() - t0 >= a.seconds:
            return 0
        time.sleep(1.0 / a.fps)


if __name__ == "__main__":
    sys.exit(main())
