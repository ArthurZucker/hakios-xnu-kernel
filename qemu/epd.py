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
  epd.py --calibrate X.png   # every ASSUMED number, and the test page at those numbers
  epd.py --set du.residue=0.35 --save   # change a number for this run (hex is accepted: grey.black=0x60)
  epd.py --demo              # the model's own self-check: no QEMU, no files, asserts, exit 0 on pass

QEMU_INSTANCE picks which copy of QEMU to read, as in qemu/stop.sh.

The numbers this model runs on live in one of two places: the literals below, or qemu/panel.json when that
file exists (it is written with exactly those literals, so the defaults are recorded somewhere you can edit
without touching Python). `--calibrate` prints the table; `--set ... --save` writes it back. `--demo` checks
that a change to a number actually changes what the panel renders, without needing QEMU.

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
# truth; these literals are only what the panel does until someone measures it. devices/b651/SIM.md.
#
# KNOBS is the single source of truth, not a copy of the tables: apply() writes KNOBS and then recomputes MODES,
# MODE_LEVELS and the three ink values FROM it. The tables used to be rebound independently, which meant a caller that
# passed only some of them left the rest stale -- and panel()'s `levels` default was bound at def time, so a grey
# change reached the tables and never reached the render. Both of those are fixed by there being one writer.
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

# qemu/panel.json is the written form of KNOBS, and it exists for one reason: these numbers are all ASSUMED, and the
# person who measures the panel will not want to edit Python to record it. It is written with the built-in defaults, so
# `--calibrate` can say where a number came from and a diff on that file is a calibration change, reviewable on its
# own. It is a local file, not a tracked one: .gitignore takes /qemu/* and lets only *.sh, *.py and *.md through, so a
# fresh clone has no panel.json and runs on the literals below. Both states are supported; the self-check only
# complains when a file is present and disagrees with KNOBS.
#
# Keys are exactly the KNOBS names above (the self-check refuses a file that has drifted), values are the same types:
#   grey.levels    int    2..      greys gc16/gl16 address; LOW makes the fade band
#   grey.black     int    0..255   of a full black; the ink's darkest state
#   grey.white     int    0..255   of a full white; above grey.black, or apply() lifts it one step
#   <mode>.frames  int    >= 1     how many panel frames that waveform takes to settle
#   <mode>.flash   bool            whether it blanks the panel (a flashing mode leaves no residue)
#   <mode>.residue float  0.0..0.9 how much of the old image survives one update; grows with history
#   du.levels      int    >= 2     per-pixel states for du; a2.levels likewise
#   panel.fps      float  > 0      how often the model repaints, so --burst is timed like the controller
#
# JSON has no comments, which is why the table is here instead of in the file. Editing panel.json by hand is supported
# (load() ignores unknown keys and refuses values that do not fit); `--set ... --save` is the supported way to write it,
# because it keeps the types and the key set honest. Delete the file to go back to the literals above.

# A panel cannot address fewer than two states per pixel: the LUT below divides by levels-1, and one level is a panel
# that can only show one colour. Clamped rather than rejected, so a calibration sweep cannot make the model crash.
MIN_LEVELS = 2


def _coerce(name, value):
    """Turn a value from the command line or panel.json into the type KNOBS[name] already has.

    Hex is accepted for the integer knobs -- `--set grey.black=0x60`, the value the panel's ink actually is -- so the
    numbers can be written in whichever base they were measured in. Python's int(s, 0) does that: 0x60, 0o140, 0b1100000
    and plain 60 all work, which `int()` alone would not (it raises on every one of the prefixes). Booleans get the same
    words argparse users type; anything else falls through to float.
    """
    current = KNOBS[name]
    if isinstance(current, bool):
        if isinstance(value, bool):
            return value
        word = str(value).strip().lower()
        if word in ("1", "true", "yes", "on"):
            return True
        if word in ("0", "false", "no", "off"):
            return False
        raise ValueError(f"{name}={value!r} is not a boolean (1/0, true/false, yes/no, on/off)")
    if isinstance(current, int):
        try:
            return int(str(value).strip(), 0)       # 0: 0x60, 0o140, 0b1100000 and plain 60 all parse
        except ValueError:
            raise ValueError(f"{name}={value!r} is not an integer (decimal, 0x hex, 0o octal or 0b binary)")
    try:
        return float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name}={value!r} is not a number")


def apply(knobs):
    """Fold `knobs` (any subset of KNOBS) into the calibration, and recompute every table that reads it.

    The tables are changed in place, never rebound: epd_live.py and epd_view.py do `from epd import MODES`, which
    captures the dict object, so a rebind here would leave them rendering the calibration from before the call. That is
    the same class of bug as panel()'s old default argument -- a name that stopped meaning what the caller thought.

    panel() takes `levels=None` for the same reason: a default of GREY_LEVELS would be bound at def time and a later
    grey.levels change would never reach the render. It reads GREY_LEVELS when called instead.

    Unknown keys are rejected rather than ignored: `--set grey.levles=8` should say so, not do nothing. Values are
    coerced to the knob's existing type, so hex and the boolean words work here too.
    """
    global GREY_LEVELS, INK_BLACK, PAPER_WHITE
    unknown = sorted(k for k in knobs if k not in KNOBS)
    if unknown:
        raise KeyError(f"no such knob(s): {', '.join(unknown)}; --calibrate prints the list")
    KNOBS.update({k: _coerce(k, v) for k, v in knobs.items()})

    GREY_LEVELS = max(MIN_LEVELS, int(KNOBS["grey.levels"]))
    INK_BLACK = min(255, max(0, int(KNOBS["grey.black"])))
    PAPER_WHITE = min(255, max(0, int(KNOBS["grey.white"])))
    if PAPER_WHITE <= INK_BLACK:
        # A panel with no span shows one flat tone; keep at least one step so the render is still readable and the LUT
        # below cannot divide by zero on `white - black == 0`.
        PAPER_WHITE = min(255, INK_BLACK + 1)

    MODES.clear()
    for mode in ("gc16", "gl16", "du", "a2"):
        MODES[mode] = (int(KNOBS[f"{mode}.frames"]), bool(KNOBS[f"{mode}.flash"]),
                       float(KNOBS[f"{mode}.residue"]))
    MODE_LEVELS.clear()
    # the grey waveforms address the full ramp; du and a2 keep their own two states
    MODE_LEVELS.update({"gc16": GREY_LEVELS, "gl16": GREY_LEVELS,
                        "du": max(MIN_LEVELS, int(KNOBS["du.levels"])),
                        "a2": max(MIN_LEVELS, int(KNOBS["a2.levels"]))})
    return dict(KNOBS)


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
    if not isinstance(saved, dict):
        print(f"epd.py: {path} is not a JSON object; using the built-in numbers", file=sys.stderr)
        return None
    knobs = {k: v for k, v in saved.items() if k in KNOBS}      # unknown keys are a newer/older file: ignore
    if not knobs:
        print(f"epd.py: {path} has none of the {len(KNOBS)} knobs; using the built-in numbers", file=sys.stderr)
        return None
    try:
        apply(knobs)
    except (KeyError, ValueError, TypeError) as e:
        print(f"epd.py: {path} does not fit the model ({e}); using the built-in numbers", file=sys.stderr)
        return None
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


def panel(img, levels=None):
    """What the panel can actually show, as opposed to what the framebuffer asked for.

    Two things a real controller does that a framebuffer does not: it snaps every pixel to one of `levels` grey
    levels, and it can only reach INK_BLACK..PAPER_WHITE, never 0..255. Luma first, because an electrophoretic pixel
    has no colour; then the level index; then the ink. `levels` is the waveform's, not the panel's: a2/du address two
    states per pixel and would otherwise be drawn with a grey ramp they cannot make (MODE_LEVELS). This is applied at
    the edge, to the picture, not to the state carried between frames: `old` stays the frame the panel was *driven*
    with (see settle), so ghosting is computed in framebuffer space and does not compound through the narrower panel
    range.

    `levels=None` means "the full ramp, as it is right now", and is read when called. A default of GREY_LEVELS would be
    evaluated at def time, which is exactly how a `--set grey.levels=8` used to change every table except the one the
    render actually used.
    """
    levels = GREY_LEVELS if levels is None else int(levels)
    levels = max(MIN_LEVELS, levels)
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
        # through. Take the darker of the two, because dark *is* ink on this panel (INK_BLACK is the pigment state) --
        # that is a property of the panel, not of the console, and it holds now that the console draws dark ink on a
        # paper-white page exactly as it held when it drew white on black. Then fade that toward the clean result by
        # the residue fraction -- scaled per pixel by how far that pixel was driven.
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

    Counting "not white" is wrong for a text console, and counting "not the framebuffer's background" is wrong too:
    which way round the console is drawn is a decision that has already changed once -- the XNU console was white ink on
    a black page and now draws dark ink on a paper-white page (video_console.c COLOR_BACKGROUND/COLOR_FOREGROUND were
    swapped for the reflective panel). Any hardcoded guess at the ink colour breaks when that flips. The background is
    the modal colour, whatever it happens to be; ink is everything else.
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


def frame_rate(asked=None):
    """The repaint rate the loops sleep on: --fps if it was given, otherwise the panel.fps calibration knob.

    panel.fps is a knob, so it has to reach the loops that repaint or it is decoration -- and until this existed it was
    exactly that: the loops read argparse's own default and the knob was only ever printed. --fps still wins when
    given, because the speed of a one-off capture is not a property of the panel.

    Raises ValueError rather than exiting, so the self-check can call it and main() can put the message on stderr.
    """
    rate = float(KNOBS["panel.fps"]) if asked is None else float(asked)
    if rate <= 0:
        raise ValueError(f"--fps must be > 0, got {rate}")
    return rate


def parse_sets(sets):
    """Turn repeated `--set k=v` strings into a dict of raw values, rejecting a name that is not a knob.

    Kept apart from apply() so the error mentions the command line the user typed, and so both the --calibrate path and
    the plain run path parse identically. The value is left as the string; apply() coerces it to the knob's type, which
    is where hex and the boolean words are understood.
    """
    knobs = {}
    for one in sets:
        key, sep, value = one.partition("=")
        if not sep:
            sys.exit(f"epd.py: --set wants KNOB=VALUE, got {one!r}. --calibrate prints the list.")
        if key not in KNOBS:
            sys.exit(f"epd.py: no knob {key!r}. --calibrate prints the list.")
        knobs[key] = value
    return knobs


def _selftest_panel_changes():
    """apply() must reach what panel() draws: the bug this replaced was a default argument frozen at def time.

    Both ends are moved to a value this check picks rather than to a constant, because the calibration loaded from
    panel.json is free to already be at any value -- a check that assumed it was not would pass on one panel.json and
    fail on another for no reason.
    """
    grey = Image.new("RGB", (16, 16), (128, 128, 128))
    before = panel(grey).getpixel((0, 0))[0]
    apply({"grey.levels": 4})
    after = panel(grey).getpixel((0, 0))[0]
    four = panel(grey, 4).getpixel((0, 0))[0]
    assert after != before, f"apply(grey.levels=4) did not change panel(): still {before}"
    assert after == four, f"panel() at the new default {after} is not panel(levels=4) {four}"
    ink_before = panel(grey).getpixel((0, 0))[0]
    darker = max(0, KNOBS["grey.black"] - 0x10)     # ink is darker than paper: pick a black that is a real change
    if darker == KNOBS["grey.black"]:
        darker = KNOBS["grey.black"] + 0x10
    apply({"grey.black": darker})
    ink_after = panel(grey).getpixel((0, 0))[0]
    assert ink_after != ink_before, f"grey.black={darker} did not reach panel(): {ink_before} either way"
    return f"levels {before} -> {after}, black {ink_before} -> {ink_after}"


def _selftest_residue():
    """Each waveform's rendered residue must agree with its own knobs.

    Measured the way the model is used: a white frame driven over a black one, at growing history, so the only variables
    are the waveform and how long the partial-update streak is. Three things are true of the model and must stay true:

      - a flashing mode re-drives every pixel and is clean at any history;
      - a non-flashing mode with residue 0.0 leaves exactly the clean image (residue is not silently always on);
      - a non-flashing mode with residue > 0 is strictly darker than clean once the streak has built up (it is not
        silently ignored). It may saturate early -- a2 at residue 0.6 reaches ink black on the first update -- which is
        why this is checked at the deepest history rather than requiring a fade.

    Plus: luminance never rises as history grows (residue accumulates, it does not wash off), and a mode with more
    residue per update is never cleaner than one with less at the same history. The mode ordering is derived from the
    knobs, not hardcoded: a recalibration is entitled to say gl16 is dirtier than du, and this should then expect it.
    """
    new = Image.new("RGB", (64, 64), (255, 255, 255))
    old = Image.new("RGB", (64, 64), (0, 0, 0))
    clean = ImageStat.Stat(panel(new).convert("L")).mean[0]
    histories = (0, 1, 2, 4, 8, 12)
    trace = {}
    for mode in ("gc16", "gl16", "du", "a2"):
        trace[mode] = [ImageStat.Stat(settle(new, old, mode, h).convert("L")).mean[0] for h in histories]
        _, flashes, residue = MODES[mode]
        if flashes:
            assert all(abs(v - clean) < 1.0 for v in trace[mode]), \
                f"{mode} flashes but left residue: {trace[mode]} vs clean {clean}"
            continue
        if residue == 0.0:
            assert all(abs(v - clean) < 1.0 for v in trace[mode]), \
                f"{mode} has residue 0.0 but is not clean: {trace[mode]} vs clean {clean}"
            continue
        assert trace[mode][-1] < clean - 1.0, \
            f"{mode} has residue {residue} but is clean at history {histories[-1]}: {trace[mode]} vs {clean}"
        assert trace[mode] == sorted(trace[mode], reverse=True), \
            f"{mode} got brighter as residue accumulated: {trace[mode]}"
    # a mode that ghosts more per update must be no cleaner once the residue has built up: more residue means more of
    # the old black survives, so the luminance can only fall. Ties are real -- du and a2 quantise to two states and both
    # can land on ink black -- which is why this is non-increasing rather than strictly decreasing.
    ordered = sorted((m for m in trace if not MODES[m][1]), key=lambda m: MODES[m][2])
    settled = [trace[m][-1] for m in ordered]
    assert all(a >= b for a, b in zip(settled, settled[1:])), \
        f"residue knobs and rendered residue disagree: {dict(zip(ordered, settled))} " \
        f"for residues {[MODES[m][2] for m in ordered]}"
    return {m: trace[m][-1] for m in trace}


def _selftest_hex_and_boxes():
    """Hex must parse, a bad knob must be refused, and a knobs dict must not have to be complete."""
    assert _coerce("grey.black", "0x60") == 0x60 == 96
    assert _coerce("grey.black", " 96 ") == 96
    assert _coerce("panel.fps", "4") == 4.0
    assert _coerce("gc16.flash", "no") is False and _coerce("gc16.flash", "1") is True
    untouched = MODES["gc16"]                                  # whatever the loaded calibration says it is
    apply({"du.residue": 0.5})                                 # one knob, not the whole KNOBS table
    assert MODES["du"][2] == 0.5, f"a partial apply() did not set du.residue: {MODES['du']}"
    assert MODES["gc16"] == untouched, \
        f"a partial apply() disturbed a mode it was not given: {MODES['gc16']} != {untouched}"
    try:
        apply({"grey.levles": 8})
    except KeyError:
        pass
    else:
        raise AssertionError("apply() accepted a misspelled knob instead of refusing it")
    try:
        _coerce("panel.fps", "fast")
    except ValueError:
        pass
    else:
        raise AssertionError("a non-numeric value for a float knob was accepted")
    return "0x60, booleans, partial apply, misspelled knob refused"


def _selftest_objects():
    """The tables are dicts the consumers already imported: apply() must mutate them, never rebind them.

    epd_live.py and epd_view.py both do `from epd import MODES`. Rebinding MODES here would leave their imported name
    pointing at the old dict -- the calibration would silently not reach the phone view. The fps check is here for the
    same reason: panel.fps is the one knob whose consumer is not a table, and a knob nothing reads is a lie in the
    table that --calibrate prints.
    """
    modes, levels = MODES, MODE_LEVELS
    apply({"a2.frames": 3})
    assert MODES is modes and MODE_LEVELS is levels, \
        "apply() rebound MODES/MODE_LEVELS; importers would keep the old one"
    assert MODES["a2"][0] == 3
    rate = frame_rate()
    assert rate == float(KNOBS["panel.fps"]), f"frame_rate() {rate} does not follow panel.fps {KNOBS['panel.fps']}"
    assert frame_rate(7.0) == 7.0, "an explicit --fps did not win over the panel.fps knob"
    apply({"panel.fps": 4.5})
    assert frame_rate() == 4.5, f"changing panel.fps did not change the repaint rate: {frame_rate()}"
    try:
        frame_rate(0)
    except ValueError:
        pass
    else:
        raise AssertionError("--fps 0 was accepted; the loops divide by it")
    return MODES["a2"]


def _selftest_calibration_file():
    """panel.json, when present, must agree with KNOBS -- or the next run silently renders a different panel.

    If they disagree, panel.json wins at import (load() runs at the bottom of the module) and the literals quietly stop
    meaning anything: nothing errors, the picture is just wrong. That is the failure this check exists for.

    An absent file is NOT a failure. It is the documented fallback (see load()), and it is also the normal state of a
    fresh clone: .gitignore tracks /qemu/* with only *.sh, *.py and *.md allowed through, so panel.json exists on the
    machine that calibrated and nowhere else. Reporting that as a broken model would make `--demo` red on every fresh
    checkout for a file that is meant to be local.
    """
    if not os.path.exists(CALIBRATION):
        return f"absent ({CALIBRATION}), using the built-in literals"
    with open(CALIBRATION) as f:
        saved = json.load(f)
    assert isinstance(saved, dict) and saved, f"{CALIBRATION} is not a non-empty JSON object"
    missing = sorted(set(KNOBS) - set(saved))
    extra = sorted(set(saved) - set(KNOBS))
    assert not missing and not extra, f"{CALIBRATION} does not match KNOBS: missing={missing} extra={extra}"
    for key, value in saved.items():
        assert type(value) is type(KNOBS[key]), \
            f"{CALIBRATION} {key}={value!r} is {type(value).__name__}, KNOBS has {type(KNOBS[key]).__name__}"
    # a JSONDecodeError is a ValueError, so the caller's except clause reports it as a failed check rather than a crash
    return f"{len(saved)} knobs agree with KNOBS"


def demo():
    """Self-check of the model. No QEMU, no files written, no network: run it after any edit to epd.py.

        python3 qemu/epd.py --demo

    Each check names the failure it is looking for. It exits non-zero with the assertion that failed, so it works as a
    CI step on a host where the simulator is not running -- which is the only way to catch a calibration change that
    renders nothing differently.

    The calibration is snapshotted once, here, and restored before every check: the checks write knobs to prove those
    knobs have an effect, and one leaving `grey.levels=4` behind would decide what the next one measures. Restoring from
    a snapshot rather than calling load() each time also means a corrupt panel.json warns once, not once per check.
    """
    checks = [
        ("apply reaches panel()", _selftest_panel_changes),
        ("each waveform leaves its own residue", _selftest_residue),
        ("hex and partial knobs", _selftest_hex_and_boxes),
        ("tables are mutated, not rebound", _selftest_objects),
        ("panel.json matches KNOBS", _selftest_calibration_file),
    ]
    base = dict(KNOBS)                     # the calibration every check starts from
    print(f"epd.py self-check: {len(checks)} checks, Pillow {__import__('PIL').__version__}")
    for n, (name, fn) in enumerate(checks, 1):
        print(f"  [{n}/{len(checks)}] {name} ... ", end="", flush=True)
        try:
            apply(base)
            detail = fn()
        except AssertionError as e:
            print("FAIL")
            print(f"\nepd.py --demo: check {n} ({name}) failed: {e}")
            apply(base)
            return 1
        except (OSError, ValueError, TypeError, KeyError) as e:
            print("FAIL")
            print(f"\nepd.py --demo: check {n} ({name}) raised {type(e).__name__}: {e}")
            apply(base)
            return 1
        print(f"ok  {detail!r}" if detail is not None else "ok")
    apply(base)             # leave the module on the calibration it imported with
    print("epd.py --demo: all checks passed")
    return 0


def calibrate(a):
    """--calibrate: the table, then the test page at those numbers. --set changes them first."""
    if a.set:
        try:
            apply(parse_sets(a.set))
        except (KeyError, ValueError) as e:
            sys.exit(f"epd.py: {e}")
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
    ap.add_argument("--fps", type=float, default=None,
                    help="how fast the panel repaints (default: the panel.fps knob, 2.0 -- e-ink, not 60)")
    ap.add_argument("--mode", default="gc16", choices=sorted(MODES) + ["auto"],
                    help="waveform; 'auto' picks per frame from the damage rect, as a controller does")
    ap.add_argument("--once", metavar="PNG", help="one settled frame, write a PNG, exit")
    ap.add_argument("--burst", metavar="DIR", help="follow a boot frame by frame, writing DIR/fNN.png, and exit "
                                                   "when the guest does. Use this: a boot is over in seconds.")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: forever)")
    ap.add_argument("--calibrate", metavar="PNG", nargs="?", const="", default=None,
                    help="print every calibration knob, and render the test page to PNG if given")
    ap.add_argument("--set", action="append", metavar="KNOB=VALUE",
                    help="change a knob for this run, e.g. --set du.residue=0.35 or --set grey.black=0x60 (repeatable)")
    ap.add_argument("--save", action="store_true", help="with --calibrate: write the knobs to qemu/panel.json")
    ap.add_argument("--demo", action="store_true",
                    help="self-check the model against its own knobs: needs no QEMU, writes nothing")
    a = ap.parse_args()

    if a.demo:
        return demo()
    if a.calibrate is not None:
        return calibrate(a)
    if a.set:
        try:
            apply(parse_sets(a.set))
        except (KeyError, ValueError) as e:
            sys.exit(f"epd.py: {e}")
    try:
        a.fps = frame_rate(a.fps)
    except ValueError as e:
        sys.exit(f"epd.py: {e}")

    if a.burst:
        return burst(a)

    q = Qemu(a.instance)
    ppm = os.path.join(os.path.dirname(os.path.abspath(__file__)), "b651-screen.ppm")
    # driven: the frame the panel was last given, the residue source. prev: what the last settle() printed, so the
    # per-frame delta has something to compare against on the second frame and after. It was never initialised, so the
    # live view raised NameError on n == 2 for anyone who ran epd.py without --once or --burst.
    shown, driven, prev, t0 = None, None, None, time.time()
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
            if prev is not None:
                diff = ImageChops.difference(shown, prev).convert("L")
                st = ImageStat.Stat(diff)
                print(f"[{n:4d}] mean|delta|={st.mean[0]:6.2f}  mode={a.mode}")
            prev = shown.copy()
        if a.seconds and time.time() - t0 >= a.seconds:
            return 0
        time.sleep(1.0 / a.fps)


if __name__ == "__main__":
    sys.exit(main())
