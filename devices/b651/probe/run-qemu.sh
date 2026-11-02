#!/bin/sh
# QEMU harness for the gate-1 probe.
#
# This is the *only* test that proves the probe's control flow is right
# (header -> branch -> string loop -> UART poll -> spin).  It cannot prove
# anything about the MT6877: QEMU virt's serial is a PL011 at 0x09000000,
# not the MT6577/16550 at 0x11002000, so the image is built with
# -DUART_BASE=0x09000000 -DUART_PL011=1 by build.sh.
#
# The image is loaded with QEMU's `-kernel`, which is the arm64 boot path:
# QEMU builds a DTB, sets it in x0, and branches to the image base + 0.
# Exactly the handoff ABL/LK performs.
#
# Usage: ./run-qemu.sh [seconds]      (default 3)
set -eu

cd "$(dirname "$0")"
SECS=${1:-3}

[ -f probe-qemu.img ] || ./build.sh

echo "[qemu] loading probe-qemu.img (PL011 @ 0x09000000), ${SECS}s timeout"
echo "[qemu] --- serial output begins ---"

# -cpu cortex-a53: the MT6877 is Cortex-A78/A55 class, but the probe uses
# nothing newer than AArch64 base + wfe.
#
# QEMU's serial goes to a file, not stdio: -nographic puts the *monitor* on
# stdin too, and a monitor reading a pipe never exits.  A file also means the
# banner is captured even though the probe never halts (it wfe-spins by
# design), so the wall clock is only an upper bound on when we stop looking.
SERIAL=$(mktemp "${TMPDIR:-/tmp}/probe-serial.XXXXXX") || exit 1

qemu-system-aarch64 \
    -M virt -cpu cortex-a53 -m 256M \
    -display none -monitor none -serial "file:$SERIAL" \
    -no-reboot -kernel probe-qemu.img \
    </dev/null >/dev/null 2>&1 &
QPID=$!

sleep "$SECS"
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

out=$(cat "$SERIAL")
rm -f "$SERIAL"

echo "[qemu] --- serial output ends ---"
echo "$out"
echo

case "$out" in
    *"HAKIOS PROBE: arm64 image ran"*)
        echo "PASS: banner came out of QEMU's serial."
        exit 0
        ;;
    *)
        echo "FAIL: banner did not appear." >&2
        echo "      (empty output above means the image never reached the UART)" >&2
        exit 1
        ;;
esac
