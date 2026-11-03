#!/bin/bash
# Watch the HiBreak Pro simulator: QEMU's own screen and the e-ink panel, side by side.
#
# TWO WINDOWS, ON PURPOSE. run-b651.sh boots with `-display none`, so QEMU has no screen of its own and the e-ink
# proxy (epd_live.py) is the only thing that can be looked at. That is also why "nothing but a grey screen" is such
# a confusing symptom: the panel is 1125x2436 of mostly white paper, the console draws the kernel's log into the top
# few hundred rows, and a window fitted to a laptop scale is a white rectangle with faint marks in it. Opening coco
# as well shows the same guest twice -- the scanout QEMU is driving, and the panel as the e-ink model settled it.
#
# usage: qemu/live.sh [INSTANCE] [SECONDS]     (default 80, 3600)
#   INSTANCE   QEMU_INSTANCE, so several can run at once; its monitor is 4444 + 10*INSTANCE
#   SECONDS    how long to run; the guest is stopped then
# env:
#   EPD_SCALE  the panel window's scale; default is the fit to this display
#   NO_PANEL=1 boot and show QEMU's screen only
#   NO_SCREEN=1 the e-ink panel only (this is what the old launcher did)
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$HERE"
N=${1:-80}; SECS=${2:-3600}
MON=$((4444 + 10 * N))
LOG=${TMPDIR:-/tmp}/hakios-live-$N

# A stale viewer from a previous run holds the monitor (QEMU allows one client), and the new one would then show the
# old guest's last frame. Kill it by its monitor port, never all of them: another instance may be running.
pkill -f "epd_live.py --monitor 127.0.0.1:$MON" 2>/dev/null

echo "qemu/live.sh: instance $N, monitor $MON, ${SECS}s"
echo "  guest log   $LOG-boot.log"
[ -n "${NO_PANEL:-}" ] || echo "  panel log   $LOG-panel.log"

DISPLAY_ARG=()
[ -n "${NO_SCREEN:-}" ] || DISPLAY_ARG=(QEMU_DISPLAY=cocoa)
env ${DISPLAY_ARG[@]+"${DISPLAY_ARG[@]}"} QEMU_INSTANCE=$N "./run-b651.sh" "$SECS" > "$LOG-boot.log" 2>&1 &

# The monitor is a TCP listener QEMU opens late in startup; its existence is how to know the guest is up. 40s of
# tries: TCG boot on this machine is about 12-20s and the port appears before the kernel does.
#
# THE WAIT DOES NOT CONNECT. The monitor serves one client at a time and the viewer is that client: a probe that
# opens a socket and closes it can still be counted as the client when the viewer connects, and the viewer then gets
# its grab refused. `lsof` asks the kernel who is listening without joining the queue.
up=0
for _ in $(seq 1 400); do
	if lsof -nP -iTCP:"$MON" -sTCP:LISTEN >/dev/null 2>&1; then up=1; break; fi
	sleep 0.1
done
if [ "$up" -eq 0 ]; then
	echo "qemu/live.sh: no monitor on $MON -- the guest did not start. Last lines of $LOG-boot.log:" >&2
	tail -20 "$LOG-boot.log" >&2
	exit 1
fi

if [ -n "${NO_PANEL:-}" ]; then
	wait
	exit $?
fi

SCALE_ARG=()
[ -n "${EPD_SCALE:-}" ] && SCALE_ARG=(--scale "$EPD_SCALE")
# The system python has tkinter (the venvs here do not) and no PIL, and /tmp is wiped on reboot, so the viewer's
# Pillow lives in a path that survives one. Set up once by:
#   /usr/bin/python3 -m pip install --target ~/.local/share/hakios/pylib pillow
PYLIB=${PYLIB:-$HOME/.local/share/hakios/pylib}
[ -d "$PYLIB" ] || PYLIB=/tmp/py39lib
[ -d "$PYLIB" ] || {
	echo "qemu/live.sh: no Pillow for /usr/bin/python3. Install it:" >&2
	echo "  /usr/bin/python3 -m pip install --target ~/.local/share/hakios/pylib pillow" >&2
	exit 1
}
# --ink crops to where the ink actually is: the panel is 1125 wide but the console uses about 300 of those pixels,
# so showing all of it is a white page with a column of text down the left. For the boot this is the whole point.
PYTHONPATH=$PYLIB /usr/bin/python3 epd_live.py \
	--monitor "127.0.0.1:$MON" --ink ${SCALE_ARG[@]+"${SCALE_ARG[@]}"} --fps 4 --seconds "$SECS" \
	2>&1 | tee "$LOG-panel.log"
