#!/usr/bin/env python3
"""Record a boot as a video: one frame every time the panel is actually addressed.

A screencast of the simulator window would show you a 4 fps window, which is not what the panel does -- the panel
sits still between updates and then flashes for a whole waveform when something changes. This asks QEMU for the
framebuffer on a fixed cadence, runs each frame through the same epd.py model the viewer uses, and writes one video
frame per sample. Fast-forwarding then plays back at the rate a person standing in front of the panel would see.

usage: record_boot.py --monitor HOST:PORT --seconds N --out boot.mp4 [--wav]
       record_boot.py --monitor HOST:PORT --seconds N --out boot.mp4 --from-log boot.log

  --from-log   start sampling only once the log shows the loader has handed over to XNU
  --wav        also write boot.wav: the serial log's own text is not sound, so this is a silent track sized to
               match, which keeps players from guessing a duration from the video alone
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import epd                       # noqa: E402
from epd import MODES, PANEL_H, PANEL_W, auto_mode, damage, settle   # noqa: E402
from PIL import Image            # noqa: E402

FPS = 4.0                        # samples a second; the panel's own rate, not the host's
SCALE = 0.505                     # what epd_live fits a 1440-tall screen to


def _instance_of(mon):
    """Qemu() takes an instance number and derives the port; accept either form on the command line."""
    if ":" in mon:
        port = int(mon.rsplit(":", 1)[1])
        if (port - 4444) % 10:
            raise SystemExit(f"record_boot: {mon} is not a 4444+10*N monitor port")
        return (port - 4444) // 10
    return int(mon)


def frames_from(mon, seconds, start_marker=None, log_path=None):
    """Yield (t, settled_image, mode_name) for `seconds`, sampling at FPS."""
    live = epd.Qemu(instance=_instance_of(mon))
    tmp_ppm = os.path.join(tempfile.gettempdir(), f"record-boot-{os.getpid()}.ppm")

    if log_path and start_marker:
        while start_marker not in open(log_path, errors="replace").read():
            time.sleep(0.5)

    driven = None
    partial = 0
    t0 = time.time()
    step = 1.0 / FPS
    while True:
        t = time.time() - t0
        if t >= seconds:
            break
        raw = live.ppm(tmp_ppm)              # PIL image of the raw framebuffer
        if raw is None:
            time.sleep(step)
            continue
        # The first frame has nothing to compare against: the panel is blank before the loader draws, so the whole
        # page is the damage. damage() needs a previous image, so say so explicitly instead of passing None.
        if driven is None:
            rect = (0, 0, raw.width, raw.height)
        else:
            rect = damage(raw, driven)
        mode = auto_mode(rect, partial)
        if mode is None:
            shown = driven if driven is not None else raw
            name = "idle"
        else:
            shown = settle(raw, driven, mode, partial, rect)
            partial = 0 if MODES[mode][1] else partial + 1
            name = mode
        driven = raw
        yield t, shown, name
        sleep = step - ((time.time() - t0) - t)
        if sleep > 0:
            time.sleep(sleep)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor", required=True, help="HOST:PORT of QEMU's monitor")
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--out", required=True, help="where to write the mp4")
    ap.add_argument("--wav", action="store_true", help="also write a silent track of the same length")
    ap.add_argument("--from-log", metavar="LOG", help="wait until the loader has handed over to XNU")
    ap.add_argument("--marker", default="Darwin Kernel Version")
    a = ap.parse_args()

    out = a.out if os.path.isabs(a.out) else os.path.abspath(a.out)
    base = os.path.splitext(out)[0]

    # The panel is 1125x2436; at 0.505 that is 568x1230, small enough to encode fast and large enough to read.
    w = max(1, round(PANEL_W * SCALE))
    h = max(1, round(PANEL_H * SCALE))
    enc = subprocess.Popen(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{w}x{h}", "-r", str(FPS), "-i", "-",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart", out],
        stdin=subprocess.PIPE)

    n = 0
    for t, img, name in frames_from(a.monitor, a.seconds, a.marker, a.from_log):
        # stretch the panel's own grey range to the panel's own range so the encode does not flatten the text
        enc.stdin.write(img.convert("L").resize((w, h), Image.Resampling.LANCZOS).tobytes())
        n += 1
        print(f"[{t:6.1f}s] frame {n:4d} {name:5s}", flush=True)
    enc.stdin.close()
    enc.wait()

    if a.wav:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
                        "-i", "anullsrc=r=48000:cl=mono", "-t", str(a.seconds),
                        base + ".wav"], check=False)

    print(f"{out}: {n} frames at {w}x{h}, {FPS:g} fps, {a.seconds:g}s", flush=True)


if __name__ == "__main__":
    main()
