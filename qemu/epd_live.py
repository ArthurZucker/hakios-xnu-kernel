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
  epd_live.py --once /tmp/frame.png            # one frame, no window: the headless check

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


def phone(panel, scale=0.40):
    """The settled panel as the device: a dark body, side bezels, a taller chin, the earpiece slit.

    Scaled by `scale` because 1125x2436 at 100% is taller than any laptop screen; 40% is ~450x975 and fits, while
    still being large enough to read the kernel's console lines, which is what you are watching.
    """
    sw = max(1, round(PANEL_W * scale))
    sh = max(1, round(PANEL_H * scale))
    screen = panel.resize((sw, sh), Image.Resampling.LANCZOS)   # LANCZOS: text stays legible when shrunk
    side = max(6, round(46 * scale))
    top = max(7, round(74 * scale))
    chin = max(8, round(104 * scale))
    radius = max(8, round(150 * scale))
    w, h = sw + 2 * side, sh + top + chin
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


def _tk_window(a):
    """(show, root) for a real window, or raise if this Python has no tkinter / no display.

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
    held = {"photo": None}               # Tk does not keep a reference: without this the label goes blank
    pending = queue.Queue(maxsize=1)

    def show(img):
        img.save(scratch, "PPM")         # P6, and Tk sniffs the format from the magic
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
    return show, root


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
        show, root = _tk_window(a)
        print(f"epd_live.py: {a.monitor}, {a.fps:g} fps, mode={a.mode}, window {TITLE!r}", flush=True)
    except Exception as e:                # ImportError (no tkinter) or TclError (no display): both mean "no window"
        if not isinstance(e, (ImportError, AttributeError)):
            print(f"epd_live.py: no window ({e.__class__.__name__}: {e})", flush=True)
        show, root = _png_window(a)

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
                    show(phone(shown, a.scale))
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
    ap.add_argument("--scale", type=float, default=0.40, help="panel scale in the window; default 0.40")
    ap.add_argument("--once", metavar="PNG", help="one settled frame to PNG, then exit; needs no window or display")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until the guest ends)")
    ap.add_argument("--frames", metavar="DIR",
                    help="where to write live.png when no window can be opened (default: a temp dir)")
    a = ap.parse_args()
    return once(a) if a.once else live(a)


if __name__ == "__main__":
    sys.exit(main())
