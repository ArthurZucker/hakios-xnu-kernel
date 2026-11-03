#!/usr/bin/env python3
"""Watch a running QEMU as the phone it is pretending to be.

`run-b651.sh` with `-display cocoa` opens a window on the raw 1125x2436 framebuffer, which is not what a HiBreak Pro
looks like: it is a rectangle of pixels with no body around it, updated at whatever rate the guest draws. The panel
is the interesting part, and the panel is not the framebuffer -- it ghosts, it updates in damage rects, it flashes on
a full clear. epd.py models that but prints numbers at a terminal; epd_view.py replays a capture as a GIF, after the
fact. This one is the third thing: live, in a window shaped like the phone, so you can watch a boot happen.

Each frame it sends `screendump` to QEMU's monitor, reads the PPM back, diffs it against the frame the panel was last
driven with, lets epd.py pick the waveform and settle the image, and draws the result inside a bezel.

usage:
  epd_live.py                                  # monitor 127.0.0.1:4444, 4 fps, waveform chosen per frame
  epd_live.py --monitor 127.0.0.1:4494         # QEMU_INSTANCE=5 (monitor is 4444 + 10*N)
  epd_live.py --mode du --fps 8                # hold one waveform and watch the ghosting pile up
  epd_live.py --scale 0.52                     # pin the window scale instead of fitting the display
  epd_live.py --scale 1.0                      # the panel at 1:1, for a pixel-exact look (needs a tall screen)
  epd_live.py --once /tmp/frame.png            # one frame, no window: the headless check

How the picture gets onto the screen is not an afterthought. The panel's own grey range is narrow by design -- ink is
not black and paper is not white (epd.INK_BLACK..epd.PAPER_WHITE) -- so drawing those numbers straight into a window
shows a washed-out grey rectangle, and the console's 8x16 glyphs are one-pixel strokes that a downscale thins further.
`_stretch` restores the panel's contrast to the window's full range and is what makes the window read as a page of
text instead of a field of grey. --scale defaults to fitting the whole body on the display, so the phone is never
taller than the screen it is being watched on.

The picture is only as honest as epd.py's model: it is the panel *behaviour* (settle time, residue, damage rects),
not the phone's real EPDC, and not its colour. See devices/b651/SIM.md, "The e-ink model".

Requires: Pillow; tkinter for the window (stdlib, but some Pythons build without it -- then it writes a PNG per
frame instead and says which file).
"""
import argparse
import os
import random
import socket
import sys
import tempfile
import threading
import time

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("epd_live.py: needs Pillow: python3 -m pip install pillow")

# The panel model lives next door and is imported, never copied: the waveforms are epd.py's, and it is edited on its
# own schedule. If it stops exporting what is needed, say which name went missing rather than failing at frame 1.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from epd import MODES, PANEL_H, PANEL_W, auto_mode, damage, settle
except ImportError as e:
    sys.exit(f"epd_live.py: qemu/epd.py does not export what this needs ({e}); it needs settle(), damage(), "
             "auto_mode(), MODES, PANEL_W and PANEL_H. If epd.py changed, this import is what to fix.")

import epd

# Optional: epd.py's own ink/background description, used only to make the status line more useful.
DESCRIBE = getattr(epd, "describe", None)

# The body around the screen. Not the phone's real bezel colour, which is a matte grey-black that photographs as
# anything; this is dark enough that the panel is the only thing your eye lands on.
SURROUND = (8, 8, 10)
BODY = (26, 26, 30)
EDGE = (58, 58, 64)
TITLE = "HiBreak Pro — e-ink"

# How the panel is sized for the window. The bezel ratios are the same numbers `phone` draws with; they are here as
# named constants so `default_scale` can predict the body's height without constructing it.
_BEZEL_TOP = 74               # top bezel at scale 1.0, before the max() floor `phone` applies
_BEZEL_CHIN = 104             # chin at scale 1.0, likewise
_WINDOW_CHROME = 120          # menu bar, dock and title bar not available to the window
MIN_SCALE = 0.20              # below this the glyphs are unreadable; a bigger window is the only fix
# Above 1:1 the panel would be inventing detail -- but the console draws 8x16 glyphs on a 1125 px-wide panel, which
# at any fit-to-display scale is a 4x8 smear. --crop + a scale over 1 is the honest way to read the text: it shows a
# real region of the panel enlarged, not a whole phone shrunk. Nothing about the pixels changes, only how many window
# pixels each one occupies.
MAX_SCALE = 4.0
_FIT_STEP = 0.005             # how finely the fit walks down after the closed-form estimate
DEFAULT_SCALE = 0.40          # the fit when no display can be asked (headless, no tkinter, --once)


class MonitorGone(Exception):
    """The monitor socket closed: QEMU exited (run-b651.sh is under `timeout`), so there is nothing to watch."""


def _tokens(raw, i):
    """Whitespace/comma-free PPM tokenizer that honours `#` comments, as both P3 and P6 headers allow them."""
    n = len(raw)
    while True:
        while i < n and raw[i:i + 1].isspace():
            i += 1
        if i < n and raw[i:i + 1] == b"#":
            while i < n and raw[i:i + 1] not in (b"\n", b"\r"):
                i += 1
            continue
        break
    j = i
    while j < n and not raw[j:j + 1].isspace():
        j += 1
    if j >= n:
        raise ValueError("PPM header truncated")
    return raw[i:j], j


def parse_ppm(raw):
    """P6 (binary) or P3 (ASCII) -> RGB Image.

    Both are here because QEMU's own screendump is P6, but a capture taken any other way (or a future QEMU) may be
    P3, and silently refusing the readable one would be a confusing way to fail. Raises ValueError on a partial file,
    which is the normal case while QEMU is still writing it.
    """
    magic = raw[:2]
    if magic not in (b"P6", b"P3"):
        raise ValueError(f"not a PPM (magic {magic!r})")
    w_tok, i = _tokens(raw, 2)
    h_tok, i = _tokens(raw, i)
    m_tok, i = _tokens(raw, i)
    w, h, maxval = int(w_tok), int(h_tok), int(m_tok)
    if w <= 0 or h <= 0 or maxval <= 0 or w > 16384 or h > 16384:
        raise ValueError(f"bad PPM header {w}x{h} maxval={maxval}")

    if magic == b"P6":
        # exactly one whitespace byte separates the header from the raster; handle an explicit CRLF too
        pos = i + (2 if raw[i:i + 2] == b"\r\n" else 1)
        pix = raw[pos:]
        if len(pix) < w * h * 3:
            raise ValueError("P6 raster short")
        # QEMU pads every row to 4 bytes -- wrong for the format, but it is what it writes -- so the row stride has
        # to be derived from the file length rather than assumed to be w*3.
        stride = len(pix) // h
        if stride < w * 3:
            raise ValueError("P6 stride too small")
        data = b"".join(pix[y * stride:y * stride + w * 3] for y in range(h))
    else:
        vals = []
        while len(vals) < w * h * 3:
            tok, i = _tokens(raw, i)
            vals.append(int(tok))
        if maxval != 255:                       # rare, but a P3 is allowed any maxval; scale to 8-bit
            vals = [min(255, v * 255 // maxval) for v in vals]
        data = bytes(vals)
    return Image.frombytes("RGB", (w, h), data)


class Monitor:
    """A QEMU HMP monitor over TCP.

    epd.py's Qemu class is bound to 127.0.0.1 and to `4444 + 10*instance`; this takes a host:port as given, so the
    same viewer works for any instance (and for a monitor forwarded over ssh). The grab is the same one
    qemu/screendump.sh does: write a PPM through the monitor, wait for it, read it, delete it.
    """

    def __init__(self, spec):
        host, sep, port = spec.rpartition(":")
        if not sep or not host or not port.isdigit():
            sys.exit(f"epd_live.py: --monitor wants HOST:PORT, got {spec!r}")
        self.spec = spec
        try:
            self.s = socket.create_connection((host, int(port)), timeout=3)
        except OSError as e:
            sys.exit(f"epd_live.py: no QEMU monitor on {spec} ({e}). Start qemu/run-b651.sh first: "
                     "its monitor is 4444 + 10*QEMU_INSTANCE.")
        self.s.settimeout(5)
        self.s.sendall(b"\n")
        time.sleep(0.2)
        self._drain()                       # the banner and the first (qemu) prompt

    def _drain(self):
        out = b""
        while True:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                break
            except OSError:
                raise MonitorGone("monitor socket closed")
            if not chunk:
                raise MonitorGone("monitor socket closed")
            out += chunk
            if b"(qemu)" in out:
                break
        return out

    def grab(self, deadline=3.0):
        """One framebuffer, or None (with the monitor's own complaint in self.last_reply)."""
        path = os.path.join(tempfile.gettempdir(),
                            f"epd-live-{os.getpid()}-{random.randrange(1 << 32):08x}.ppm")
        try:
            self.s.sendall(f"screendump {path}\n".encode())
        except OSError as e:
            raise MonitorGone(str(e))
        t_end = time.time() + deadline
        img = None
        # QEMU creates the file and then fills it, so a read can catch a half-written header: poll until it parses.
        # An empty file is the normal first few milliseconds, not an error.
        while time.time() < t_end:
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            except OSError:
                raw = b""
            if raw[:2] in (b"P6", b"P3"):
                try:
                    img = parse_ppm(raw)
                    break
                except ValueError:
                    pass                     # partial: let QEMU finish
            time.sleep(0.02)
        try:
            os.unlink(path)
        except OSError:
            pass
        self.last_reply = self._drain()
        return img


def panel_frame(img):
    """The grabbed mode, placed on the panel.

    The guest does not always drive 1125x2436: UEFI runs 800x600 until the loader switches the mode. The panel is
    still the panel, so a smaller mode is drawn at the panel's top-left (which is what epd.py's crop does) with the
    rest black -- not scaled up to fill the phone, which would invent pixels the panel never had.
    """
    if img.size == (PANEL_W, PANEL_H):
        return img
    canvas = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    canvas.paste(img.crop((0, 0, min(PANEL_W, img.width), min(PANEL_H, img.height))), (0, 0))
    return canvas


def panel_range():
    """The panel's real grey range as (dark, light), read from epd each time.

    epd's prefix is its own and its constants are module globals that calibration can replace, so this reads them
    live rather than importing INK_BLACK/PAPER_WHITE at import time -- a by-value import would pin the literals and
    a saved calibration would then stop affecting the viewer, which is the one place a mismatch is visible.

    This is deliberately NOT a measurement of the frame on screen. Stretching each frame by its own measured min/max
    would turn a mostly-blank screen (a clear console, or the black before the loader switches mode) into full
    black-and-white noise, and the panel would appear to change contrast as content scrolled. The panel's contrast
    is fixed by the panel, so the mapping is fixed too.
    """
    return int(epd.INK_BLACK), int(epd.PAPER_WHITE)


def _stretch(img):
    """Map the panel's grey range onto full 0..255, so the window shows contrast rather than absolute grey.

    This is the difference between the correct colours and a legible window. epd's panel can only reach, say,
    0x22..0xDD; drawn as-is that is a narrow band of grey in the middle of the window's range, and it is exactly what
    made the old viewer read as "nothing but grey". Mapping dark->0 and light->255 puts the ink on the window's
    black and the paper on its white, which is how a paper-like panel is meant to look under room light.

    The input can be either L or RGB (epd.settle returns RGB), so the LUT is built for the image's own band count.
    Values outside the panel's range -- the resampler's ringing can overshoot by a few counts, and a caller may hand
    in anything -- are clipped to 0..255 rather than wrapped, so a border cannot appear as a bright seam.
    """
    # A stretched range here is expected to be non-zero; a zero-width panel would mean every pixel the same, which
    # has no contrast to restore, so it is passed through unchanged instead of dividing by zero.
    lo, hi = panel_range()
    if hi <= lo:
        return img
    lut = [max(0, min(255, round((v - lo) * 255.0 / (hi - lo)))) for v in range(256)]
    return img.point(lut * len(img.getbands()))


def ink_box(panel, threshold=128):
    """The rectangle the ink actually occupies, or the whole panel when nothing is drawn.

    A fixed crop is the wrong default for a console: the longest line this kernel prints is about 300 px of a
    1125 px-wide panel, so a crop of the full width leaves three quarters of the window as blank paper and the text
    reads as a thin strip on the left -- which is indistinguishable from "the screen is grey". Cropping to the ink
    is what makes the window show the text at a size a person can read.

    Measured on the panel's own greys, not on the stretched render: the panel holds only INK_BLACK..PAPER_WHITE, so
    the midpoint of *that* range is the ink/paper boundary for any calibration, where 128 would be wrong if the
    range moved.
    """
    lo, hi = panel_range()
    split = (lo + hi) // 2
    grey = panel.convert("L")
    # White the ink, black the paper: getbbox() reports the box of NON-ZERO pixels, so the mask has to make the ink
    # the non-zero one. Doing it the other way round measures the paper, which is nearly the whole panel and is how
    # this first came out as a full-height window.
    ink = grey.point(lambda v: 255 if v < split else 0)
    box = ink.getbbox()
    if not box:
        return (0, 0, panel.width, panel.height)
    x0, y0, x1, y1 = box

    # ... AND THEN DROP THE OUTLIERS, because one stray pixel in the far right margin sets x1 to the panel's edge and
    # the whole crop is full width again -- which caps the scale at fit/1125 and puts every glyph back to 1.1x, the
    # unreadable case this function exists to prevent. A column of real text holds tens to hundreds of ink pixels; a
    # cursor left behind or a corner mark holds one or two. So a column (row) counts as content when it holds at
    # least a twentieth of the densest one, and the crop keeps from the first such column to the last.
    def _trim(counts, floor_fraction=0.05):
        busiest = max(counts) if counts else 0
        if busiest == 0:
            return None
        floor = max(1, busiest * floor_fraction)
        live = [i for i, n in enumerate(counts) if n >= floor]
        return (live[0], live[-1] + 1) if live else None

    columns = [0] * (x1 - x0)
    rows = [0] * (y1 - y0)
    data = ink.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            if data[x, y]:
                columns[x - x0] += 1
                rows[y - y0] += 1
    xspan, yspan = _trim(columns), _trim(rows)
    if xspan:
        x0, x1 = x0 + xspan[0], x0 + xspan[1]
    if yspan:
        y0, y1 = y0 + yspan[0], y0 + yspan[1]

    # A little air, so glyph edges are not flush against the window, and never an empty rect on a 1-px mark.
    pad = 8
    return (max(0, x0 - pad), max(0, y0 - pad),
            min(panel.width, x1 + pad), min(panel.height, y1 + pad))


def view(panel, a):
    """What the window should show, at a size the window can actually display.

    Cropping to the ink makes the text readable but can make the image TALLER than the screen -- the console fills
    about 1170 rows of the panel, and at scale 2 that is 2340 px of image in a 1440 px screen. The window manager
    then clips it, showing only the top of the page, which is blank paper: that is the "grey screen", and nothing
    about it looks like a sizing problem. So the scale is computed from the crop and the screen, not asked for.

    `a.scale` stays an upper bound rather than a command when the result would not fit: a caller who asks for 2 and
    has room for 1.4 gets 1.4, and is told the number in force (live() prints it). Asking for a scale that cannot
    be shown and silently showing a cropped part of it is the failure this replaces.

    WHEN THE PAGE IS TALLER THAN THE SCREEN, SHOW THE END OF IT, NOT A SHRUNKEN WHOLE. The console writes downward,
    so the lines a person wants are the last ones; shrinking the whole page to fit turns 8x16 glyphs into a texture
    you can see but not read, which is what "I still can't see anything that works" looks like. So the crop keeps
    its scale and loses its top: the bottom `fit` pixels of the page, at the scale that was asked for. A page that
    fits is shown whole, as before.
    """
    if not (a.crop or a.ink):
        return phone(panel, a.scale)
    x0, y0, x1, y1 = a.crop or ink_box(panel)
    width, height = max(1, x1 - x0), max(1, y1 - y0)
    scale = a.scale
    if a.fit:
        # Width is the only hard limit: the crop's columns all have to be on screen. Height is not, because a page
        # taller than the window is shown from its bottom (below), so shrinking to fit the whole of it would be
        # giving up legibility to show lines a person has already scrolled past.
        scale = min(scale, a.fit / width)
    scale = max(MIN_SCALE, scale)
    shown = a.fit / scale if a.fit else height
    if height > shown:
        y0 = y1 - int(shown)                 # the newest lines, which are at the bottom
    return region(panel, (x0, int(y0), x1, y1), scale)


def region(panel, crop, scale):
    """`crop` (x0,y0,x1,y1) of the panel at `scale`, nearest-neighbour, with no phone body.

    The console's cells are 8x16 px (iso_font.c ISO_CHAR_WIDTH/HEIGHT) on a 1125 px-wide panel, so a whole-phone
    render at any scale that fits a laptop makes every glyph a 4x8 smear and there is nothing to read. This is the
    same panel pixels, just a smaller rectangle of them drawn bigger -- NEAREST because the source is 1-bit ink and
    interpolating it would invent greys the panel cannot produce.

    Both the panel's grey range and the crop are honoured: `_stretch` first so the ink is black and the paper white,
    then the crop, then the resize.
    """
    x0, y0, x1, y1 = crop
    x0, x1 = sorted((max(0, x0), min(panel.width, x1)))
    y0, y1 = sorted((max(0, y0), min(panel.height, y1)))
    if x1 <= x0 or y1 <= y0:
        return panel
    piece = _stretch(panel).crop((x0, y0, x1, y1))
    return piece.resize((max(1, round(piece.width * scale)), max(1, round(piece.height * scale))),
                        Image.Resampling.NEAREST)


def phone(panel, scale=DEFAULT_SCALE):
    """The settled panel as the device: a dark body, side bezels, a taller chin, the earpiece slit.

    The contrast fix is `_stretch`, and it is the one that actually explains the old "nothing but grey" window: the
    panel's own grey range is narrow (epd.INK_BLACK..PAPER_WHITE, 0x22..0xDD by default), so drawing those numbers
    straight into the window showed a mid-grey rectangle no matter how well the text had been resampled.

    The resampler stays a smooth one (LANCZOS). The obvious alternative for 1-px strokes is an area average (BOX),
    which preserves the exact mean of every source block and looks right on a synthetic block pattern -- but at the
    fractional scales a fitted window actually uses, BOX's blocks alternate between one and two source pixels, so a
    stroke that falls in a two-pixel block is half-diluted while its neighbour stays solid. Measured on the console's
    real one-pixel strokes that shows up as a stroke grey of 13 with a standard deviation of 57 at 0.78 (dotted,
    visibly beaded) against 70/44 for LANCZOS. LANCZOS also wins on per-glyph ink/paper separation (142 vs 139 at
    0.505) and on correlation with the true 1:1 panel. Its ringing is the price of keeping the stroke continuous, and
    against paper-white that ringing is mild.

    Scale is a plain float because the default is a fit to the display (see `default_scale`); integer scales are not
    special-cased, since the resampler is the same at every scale and 1.0 is already exact.
    """
    screen = _stretch(panel).resize((max(1, round(PANEL_W * scale)), max(1, round(PANEL_H * scale))),
                                    Image.Resampling.LANCZOS)
    side = max(6, round(46 * scale))
    top = max(7, round(74 * scale))
    chin = max(8, round(104 * scale))
    radius = max(8, round(150 * scale))
    w, h = screen.width + 2 * side, screen.height + top + chin
    body = Image.new("RGB", (w, h), SURROUND)
    d = ImageDraw.Draw(body)
    d.rounded_rectangle((2, 2, w - 3, h - 3), radius=radius, fill=BODY, outline=EDGE,
                        width=max(1, round(3 * scale)))
    body.paste(screen, (side, top))
    # top band: earpiece slit and camera dot, so the bezel reads as a phone and not as padding
    slit = max(10, round(170 * scale))
    sy = max(3, top // 2)
    d.rounded_rectangle((w // 2 - slit // 2, sy, w // 2 + slit // 2, sy + max(2, round(9 * scale))),
                        radius=max(1, round(4 * scale)), fill=(10, 10, 12))
    r = max(2, round(11 * scale))
    cx, cy = w // 2 + slit // 2 + max(10, round(46 * scale)), sy + max(1, round(4 * scale))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(14, 14, 18), outline=(46, 48, 54))
    return body


def default_scale(screen_h=None):
    """The largest scale whose whole phone body still fits the display it is being watched on.

    A fixed default cannot be right everywhere: at the old 0.40 a 1440-tall laptop had room for the phone and showed
    it small enough that the console's 8x16 glyphs were a 3x6-pixel smear, and 1125x2436 at 0.55 is a 1438-tall
    window that does not fit any laptop at all. The fit is computed from the screen, with the window chrome
    subtracted, so the same invocation is readable on a 900-tall panel and large on a 1440-tall one.

    `screen_h` is passed in by `live()` from the root it already created, rather than this function opening a Tk
    root of its own to ask: two Tk instances in one process are a thing to avoid, and the window that will show the
    phone is the honest thing to measure. With no window at all (`--once`, no display, no tkinter) the height is
    unknown, and the documented DEFAULT_SCALE is used. The user's own --scale always wins over this.
    """
    if not screen_h or screen_h <= 0:
        return DEFAULT_SCALE
    usable = screen_h - _WINDOW_CHROME
    if usable <= 0:
        return DEFAULT_SCALE
    # Closed form first, then walked down for the rounding in `phone`'s bezel arithmetic: the fit must be checked
    # against the body that will actually be built, not against the panel alone.
    scale = usable / (PANEL_H + _BEZEL_TOP + _BEZEL_CHIN)
    # A fitted whole phone is capped at 1:1 even though --scale allows more: past 1 the LANCZOS enlargement invents
    # greys between ink and paper, which is wrong for a 1-bit panel. --crop is the way past 1, and it uses NEAREST.
    scale = max(MIN_SCALE, min(1.0, scale))
    while scale > MIN_SCALE and _phone_extent(scale)[1] > usable:
        scale -= _FIT_STEP
    return round(scale, 3)


def _phone_extent(scale):
    """The (width, height) `phone` will produce at a given scale, so the fit can be tested without drawing it."""
    return (max(1, round(PANEL_W * scale)) + 2 * max(6, round(46 * scale)),
            max(1, round(PANEL_H * scale)) + max(7, round(74 * scale)) + max(8, round(104 * scale)))


def _tk_window(a):
    """(show, root, screen_height) for a real window, or raise if this Python has no tkinter / no display.

    The screen height comes back with the window because the default scale is a fit to it, and this root is the only
    display there is to ask: opening a second Tk root just to call winfo_screenheight() is how a process ends up with
    two Tcl interpreters that disagree.

    The frame is handed to Tk as a PPM on disk rather than through PIL.ImageTk: some Pillow builds ship without the
    _imagingtk extension (`invalid command name "PyImagingPhoto"`) and PhotoImage would then only fail at the first
    repaint, after the window was already up. Tk reads PPM itself, at about a millisecond a frame, so this route has
    no optional C extension in it at all. PPM is also what QEMU already speaks, so nothing new is being invented.

    `show` only *posts* the picture: Tk is not thread-safe, and `root.update()` called from the grabbing thread
    deadlocks on this build (the Tcl notifier is already inside its own event wait), so the window is driven by
    mainloop() and a queue, the way Tk expects to be driven.
    """
    import queue
    import tkinter as tk

    root = tk.Tk(className="epd-live")
    root.title(TITLE)
    root.configure(bg="#%02x%02x%02x" % SURROUND)
    root.resizable(False, False)
    label = tk.Label(root, bd=0, highlightthickness=0, bg="#%02x%02x%02x" % SURROUND)
    label.pack()
    scratch = os.path.join(tempfile.mkdtemp(prefix="epd-live-tk-"), "frame.ppm")
    held = {"photo": None, "announced": None}   # Tk does not keep a reference: without the photo the label blanks
    pending = queue.Queue(maxsize=1)

    def show(img):
        img.save(scratch, "PPM")         # P6, and Tk sniffs the format from the magic
        # Say the frame's size once. A window whose image is taller than the display gets clipped or shrunk by the
        # window manager with no message anywhere, and the symptom is a window that shows part of a page or a
        # smaller-looking one -- both indistinguishable from "the panel is wrong". Printing it makes the geometry
        # something to read rather than to infer.
        if held["announced"] != img.size:
            held["announced"] = img.size
            print(f"epd_live.py: window image {img.width}x{img.height}", flush=True)
        try:
            pending.put_nowait(scratch)  # a newer frame replaces one not yet drawn: the panel is the latest state
        except queue.Full:
            pass

    def drain():
        try:
            path = pending.get_nowait()
        except queue.Empty:
            pass
        else:
            held["photo"] = tk.PhotoImage(file=path, format="ppm")
            label.configure(image=held["photo"])
        if getattr(root, "_epd_done", False):
            root.quit()                  # the grab loop finished: leave mainloop on the thread that owns Tk
            return
        root.after(50, drain)            # 20 Hz: faster than the panel, so a frame is never held back by the window

    root._epd_done = False
    root.after(50, drain)
    # The height goes back with the window so the default scale can fit this display without opening a second Tk root.
    return show, root, root.winfo_screenheight()


def _png_window(a):
    """(show, None) when there is no window to open: write the phone render to one file and print its path.

    Headless, over ssh, or on a Python built without tkinter, the alternative is a silent tool that shows nothing.
    Printing the path each frame (rather than once) means a previewer or `open -a Preview` can be reloaded and still
    show the latest frame.
    """
    d = a.frames or tempfile.mkdtemp(prefix="epd-live-")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "live.png")
    print(f"epd_live.py: no tkinter window available, writing each frame to {path}", flush=True)

    def show(img):
        img.save(path, "PNG")
        print(f"  -> {path}  {img.width}x{img.height}", flush=True)
    return show, None


def once(a):
    """One grab, one settled frame, one PNG. No window, no display, no tkinter: this is the headless check."""
    q = Monitor(a.monitor)
    raw = q.grab()
    if raw is None:
        reply = q.last_reply.decode("utf-8", "replace").strip()
        print(f"epd_live.py: the monitor at {a.monitor} produced no frame ({reply or 'no reply'})", flush=True)
        return 1
    frame = panel_frame(raw)
    rect = (0, 0, frame.width, frame.height)          # no previous frame: a new scan drives the whole panel
    mode = a.mode if a.mode != "auto" else auto_mode(rect, 0)
    shown = settle(frame, None, mode, 0, None)
    os.makedirs(os.path.dirname(os.path.abspath(a.once)), exist_ok=True)
    shown.save(a.once, "PNG")
    extra = f"  {DESCRIBE(shown)}" if DESCRIBE else ""
    print(f"{a.once}  {shown.width}x{shown.height}  grabbed={raw.width}x{raw.height}  mode={mode}  "
          f"rect=0,0-{rect[2]},{rect[3]}{extra}", flush=True)
    return 0


def live(a):
    """Follow a running guest, settling each frame the way epd.py's burst loop does, and show it in the body.

    Two threads when there is a window: Tk owns the main one (its notifier deadlocks if `update()` is called from a
    thread that is not the one that made the window), and the grab loop runs beside it, posting frames to the window.
    With no window there is nothing to own the main thread, so the loop runs there and the code is the same.
    """
    q = Monitor(a.monitor)
    try:
        show, root, screen_h = _tk_window(a)
        # --scale is resolved after the root exists, because the default is "fit this display" and this root is the
        # only display there is. Resolving it here (rather than in main()) means `once` and the no-window path get the
        # documented constant and a window gets the fit, without either needing to open a second Tk root.
        if a.scale is None:
            a.scale = default_scale(screen_h)
        # How much of the screen the fitted image may use, so view() can cap a --crop/--ink enlargement that would
        # otherwise be taller than the display and get clipped to its blank top.
        a.fit = None
        if (a.crop or a.ink) and screen_h:
            a.fit = max(1, screen_h - _WINDOW_CHROME)
        # Say the scale that is actually in force, not the one that was asked for: the default is a fit, and the fit
        # is the number you need when the window looks wrong.
        print(f"epd_live.py: {a.monitor}, {a.fps:g} fps, mode={a.mode}, scale={a.scale:.3f}, "
              f"window {TITLE!r}", flush=True)
    except Exception as e:                # ImportError (no tkinter) or TclError (no display): both mean "no window"
        # Say it every time, ImportError included. This used to suppress ImportError quietly, so a Python without
        # tkinter (the venv here) wrote PNGs to a temp dir and printed nothing about it -- the tool looked like it
        # was running, and there was no window and no error to say why.
        print(f"epd_live.py: no window ({e.__class__.__name__}: {e}). This Python has no usable tkinter; "
              f"use /usr/bin/python3 for a window", flush=True)
        show, root = _png_window(a)
        a.fit = None
        if a.scale is None:
            a.scale = DEFAULT_SCALE       # no window: nothing to fit, and the PNGs are written at this scale

    if root is None:
        return follow(a, q, show)

    # The window owns the main thread from here. A thread does the grabbing; mainloop() draws whatever it posted.
    result = {"rc": 0}

    def work():
        try:
            result["rc"] = follow(a, q, show)
        finally:
            root._epd_done = True        # drain() sees this, draws the last frame, and quits mainloop
    threading.Thread(target=work, daemon=True).start()
    try:
        root.mainloop()
    except KeyboardInterrupt:
        pass
    return result["rc"]


def follow(a, q, show):
    """The grab loop: read the monitor, settle what moved, hand the phone render to `show`."""
    fixed = None if a.mode == "auto" else a.mode
    driven = None                         # the frame the panel was last *driven* with: the residue source
    shown = None                          # what the panel holds after settling: what the window shows
    partial = 0                           # partial updates since the last full clear
    samples = updates = 0
    t0 = t_last = t_idle = time.time()
    try:
        while True:
            t_frame = time.time()
            raw = q.grab()
            if raw is None:
                reply = q.last_reply.decode("utf-8", "replace").strip()
                print(f"[{time.time() - t0:6.1f}s] no frame from the monitor ({reply or 'no reply'})", flush=True)
                return 1
            samples += 1
            frame = panel_frame(raw)
            if driven is None or driven.size != frame.size:
                rect = (0, 0, frame.width, frame.height)      # a new scan: every pixel is addressed
            else:
                rect = damage(frame, driven)                  # None: nothing moved
            mode = fixed if fixed else auto_mode(rect, partial)
            if mode is None:
                # a controller is not given "here is a frame"; with no damage it is not addressed at all. Say so at
                # most once a second: at 4 fps a steady screen would otherwise be four lines a second of nothing,
                # which buries the updates that matter.
                if time.time() - t_idle >= 1.0:
                    print(f"[{time.time() - t0:6.1f}s] s={samples:04d} idle  rect=none  partial={partial}  "
                          f"(panel not addressed, {samples - updates} samples with no damage)", flush=True)
                    t_idle = time.time()
            else:
                shown = settle(frame, driven, mode, partial, rect if fixed is None else None)
                partial = 0 if MODES[mode][1] else partial + 1
                updates += 1
                # residue comes from the frame the panel was driven with, and outside the rect it was not driven at
                # all, so that region still holds the older frame -- exactly epd.py's burst bookkeeping
                nxt = frame
                if fixed is None and not MODES[mode][1] and driven is not None and rect is not None:
                    nxt = frame.copy()
                    nxt.paste(driven.crop(rect), rect)
                driven = nxt
                try:
                    show(view(shown, a))
                except Exception as e:
                    # closing the Tk window makes the next update() raise; that is the user saying stop, not a bug
                    print(f"[{time.time() - t0:6.1f}s] the window closed ({e.__class__.__name__}). "
                          f"{updates} updates from {samples} samples.", flush=True)
                    return 0
                where = (f"rect={rect[0]},{rect[1]}-{rect[2]},{rect[3]}" if fixed is None and rect
                         else "whole panel")
                extra = f"  {DESCRIBE(shown)}" if DESCRIBE else ""
                print(f"[{time.time() - t0:6.1f}s] f{updates:03d} s={samples:04d} {mode:4s} {where} "
                      f"partial={partial}{extra}", flush=True)
            if a.seconds and time.time() - t0 >= a.seconds:
                print(f"{updates} updates from {samples} samples in {time.time() - t0:.1f}s", flush=True)
                return 0
            t_last = time.time()
            time.sleep(max(0.0, 1.0 / a.fps - (t_last - t_frame)))
    except MonitorGone as e:
        print(f"[{time.time() - t0:6.1f}s] the monitor closed ({e}): the guest ended. "
              f"{updates} updates from {samples} samples.", flush=True)
        return 0
    except KeyboardInterrupt:
        print(f"\n{updates} updates from {samples} samples in {time.time() - t0:.1f}s", flush=True)
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--monitor", default="127.0.0.1:4444",
                    help="HOST:PORT of QEMU's monitor; default 127.0.0.1:4444 (4444 + 10*QEMU_INSTANCE)")
    ap.add_argument("--fps", type=float, default=4.0, help="panel updates per second; default 4 (e-ink, not 60)")
    ap.add_argument("--mode", default="auto", choices=sorted(MODES) + ["auto"],
                    help="waveform; 'auto' picks per frame from the damage rect, as a controller does")
    ap.add_argument("--scale", type=float, default=None,
                    help=f"panel scale in the window, 1125x2436 -> that many pixels; default fits the display "
                         f"(about {DEFAULT_SCALE:.2f} with no display to ask). 1.0 is the panel at 1:1; with --crop, "
                         f"2-4 is what makes the 8x16 console text readable")
    ap.add_argument("--ink", action="store_true",
                    help="crop to the rectangle the ink occupies and enlarge it. This is the readable default for a "
                         "console: the kernel's longest line is about 300 px of the 1125 px-wide panel, so anything "
                         "wider is mostly blank paper. Implies --scale 2 unless one is given")
    ap.add_argument("--crop", metavar="X0,Y0,X1,Y1",
                    help="show this rectangle of the panel instead of the whole phone, enlarged by --scale. The "
                         "console's glyphs are 8x16 px, so this is how to read them: --crop 0,0,300,1200 --scale 3")
    ap.add_argument("--once", metavar="PNG", help="one settled frame to PNG, then exit; needs no window or display")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until the guest ends)")
    ap.add_argument("--frames", metavar="DIR",
                    help="where to write live.png when no window can be opened (default: a temp dir)")
    a = ap.parse_args()
    # --scale left as None means "fit the display". It is deliberately NOT resolved here: the fit needs the screen
    # height, which `live()` reads from the Tk root it is about to create. `once` never scales at all -- it writes the
    # settled panel at 1125x2436, which is the honest headless artifact and matches what epd.py and epd_view.py emit.
    if a.scale is not None and a.scale <= 0:
        ap.error(f"--scale must be positive, got {a.scale:g}")
    if a.crop:
        try:
            a.crop = tuple(int(v) for v in a.crop.split(","))
        except ValueError:
            ap.error(f"--crop wants four integers X0,Y0,X1,Y1, got {a.crop!r}")
        if len(a.crop) != 4:
            ap.error(f"--crop wants four integers X0,Y0,X1,Y1, got {len(a.crop)}")
    if (a.crop or a.ink) and a.scale is None:
        a.scale = 2.0                    # a crop is for reading text; 2 is the smallest useful enlargement
    return once(a) if a.once else live(a)


if __name__ == "__main__":
    sys.exit(main())
