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
import os
import socket
import sys
import time

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    sys.exit("epd.py: needs Pillow: python3 -m pip install pillow")

# The panel, from the phone's device tree node /eink (see devices/b651/PHONE.md). The values were not
# readable without root, so the geometry here is what the SoC's e-ink panels use, not what this unit
# is confirmed to have. Fix these when the dtbo is read.
PANEL_W, PANEL_H = 824, 1648          # the screencaps from this unit came out at exactly this

# Waveform families an e-ink panel supports. Each is (frames to settle, whether it flashes, ghost residue
# after the update). The numbers are a plausible model of the named modes, not measurements.
MODES = {
    "gc16": (24, True, 0.00),         # full grey-level clear: slow, flashes, leaves nothing
    "gl16": (16, False, 0.06),        # grey-level, no flash: what GL16 means in eink_glx16_disable
    "du":   (6,  False, 0.22),        # direct update: fast, leaves a shadow of the old image
    "a2":   (2,  False, 0.45),        # animation: fastest and dirtiest
}


def monitor(path, instance=0):
    """The QEMU monitor socket for this instance."""
    return path, 4444 + 10 * instance


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


def settle(new, old, mode, history):
    """What the panel shows after driving `new` with waveform `mode`.

    Three effects, in the order a real controller applies them:
      1. a full (flashing) mode blanks the panel first, so the whole screen goes black then white
      2. pixels arrive over several frames, so a partial capture sees a mix of old and new
      3. a non-flashing mode leaves residue: the previous image at a fraction of its contrast
    """
    frames, flashes, ghost = MODES[mode]
    if old is None or old.size != new.size:
        # a new mode is a new panel scan: there is nothing to ghost over, the controller re-drives every pixel
        return new.copy()
    if flashes:
        # a flash is a full invert pass, then the new image; model it as mostly-new with some old
        out = Image.blend(new, old, 0.25)
    else:
        out = Image.blend(new, old, ghost)
    return out


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
            shown = settle(frame, prev, a.mode, n)
            n += 1
            path = os.path.join(a.burst, f"f{n:02d}.png")
            to_png(shown, path)
            cols = shown.getcolors(1 << 20) or []
            tot = sum(c for c, _ in cols)
            dark = sum(c for c, p in cols if p != (255, 255, 255))
            print(f"[{time.time() - t0:5.1f}s] f{n:02d} {shown.width}x{shown.height} "
                  f"colours={len(cols):3d} ink={100.0 * dark / tot:5.1f}%")
            prev = shown.copy()
        time.sleep(1.0 / a.fps)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", type=int, default=int(os.environ.get("QEMU_INSTANCE", 0)))
    ap.add_argument("--fps", type=float, default=2.0)
    ap.add_argument("--mode", default="gc16", choices=sorted(MODES))
    ap.add_argument("--once", metavar="PNG", help="one settled frame, write a PNG, exit")
    ap.add_argument("--burst", metavar="DIR", help="follow a boot frame by frame, writing DIR/fNN.png, and exit "
                                                   "when the guest does. Use this: a boot is over in seconds.")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: forever)")
    a = ap.parse_args()

    if a.burst:
        return burst(a)

    q = Qemu(a.instance)
    ppm = os.path.join(os.path.dirname(os.path.abspath(__file__)), "b651-screen.ppm")
    shown, t0 = None, time.time()
    n = 0
    while True:
        raw = q.ppm(ppm)
        if raw is not None:
            frame = crop(raw)
            shown = settle(frame, shown, a.mode, n)
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
