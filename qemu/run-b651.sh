#!/bin/bash
# Boot HakiOS the way the HiBreak Pro would: MT6877-shaped machine, serial console, and an e-ink panel
# instead of a framebuffer that holds its content.
#
# usage: run-b651.sh [seconds] [extra qemu args...]
#
#   QEMU_INSTANCE   which copy of QEMU this is (monitor on 4444 + 10*N); default 0 -> qemu/instance
#   QEMU_EPD_FPS    how often the panel model repaints, in Hz (default 2: an e-ink panel is not 60 Hz)
#   QEMU_MEM        guest RAM (default 4G: the phone has 8 GB and ZRAM; 4G boots faster)
#   QEMU_SMP        cores; default 8, the phone's 2x A76 + 6x A55 (need not be a number QEMU likes
#                   for its own topology -- -smp 8 on virt is fine)
#   QEMU_ESP=1      payload is a file on the ESP rather than QEMU's loader device (as run.sh)
#   QEMU_EFI, QEMU_VARS   UEFI firmware and its NVRAM template
#
# What is NOT the same as the phone, and cannot be:
#   - no MediaTek BROM/preloader, so no `fastboot flash boot` path. This machine starts from UEFI directly.
#   - no MT6877 UFS, PMIC, ADSP or modem. Substitute virtio devices stand in so the kernel's generic
#     paths have something to talk to; drivers written against the real MTK blocks will not bind.
#   - the e-ink panel is a MODEL (see epd.py), not the phone's controller. It is there so the refresh
#     state machine and the driver ABI can be developed and watched, not so that colour is exact.
cd "$(dirname "$0")"
T=${1:-30}; shift
N=${QEMU_INSTANCE:-$(cat ../qemu/instance 2>/dev/null || echo 0)}
MON=$((4444 + 10 * N))

EFI=${QEMU_EFI-}; VARS_SRC=${QEMU_VARS-}
[ -n "$EFI" ] || for f in /opt/homebrew/share/qemu/edk2-aarch64-code.fd /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd; do
	[ -f "$f" ] && { EFI=$f; break; }
done
[ -f "$EFI" ] || { echo "run-b651.sh: no aarch64 UEFI firmware; set QEMU_EFI"; exit 1; }

# Homebrew ships aarch64 code but no aarch64 vars template, so make one: a zeroed 64 MiB NVRAM, which is
# exactly what edk2 expects at the end of the code image's flash region. Made once, reused after.
# Everything this script writes lives in $STATE (qemu/qemu-b651/), never next to the sources.
STATE=qemu-b651
VARS=$STATE/vars.fd
mkdir -p "$STATE/esp/EFI/BOOT"
if [ ! -f "$VARS" ]; then
	if [ -n "$VARS_SRC" ] && [ -f "$VARS_SRC" ]; then cp "$VARS_SRC" "$VARS"
	else dd if=/dev/zero of="$VARS" bs=1m count=64 status=none; fi
fi

L=../xnuloader/loader.efi; [ -f $L ] || L=../prebuilt/loader.efi
[ -f "$L" ] || { echo "run-b651.sh: no loader ($L); run make loader-open"; exit 1; }
cp "$L" "$STATE/esp/EFI/BOOT/BOOTAA64.EFI"

PAYLOAD_DEVICE=(-device loader,file=payload.bin,addr=0xF0000000,force-raw=on)
[ -f payload.bin ] || PAYLOAD_DEVICE=()
if [ -n "${QEMU_ESP:-}" ] && [ -f payload.bin ]; then
	mkdir -p "$STATE/esp/EFI/hakios" && cp payload.bin "$STATE/esp/EFI/hakios/payload.bin"
	PAYLOAD_DEVICE=()
fi

# -cpu cortex-a76: the big cores are A76 per the phone's own device tree. max instead would let QEMU
# expose features the real A76 lacks, which flatters the port.
timeout "$T" qemu-system-aarch64 \
	-M virt,gic-version=3 \
	-cpu cortex-a76 \
	-smp "${QEMU_SMP:-8}" \
	-m "${QEMU_MEM:-4G}" \
	-accel tcg,tb-size="${QEMU_TB_MB:-512}" \
	-drive if=pflash,format=raw,readonly=on,file="$EFI" \
	-drive if=pflash,format=raw,file="$VARS" \
	-drive file=fat:rw:$STATE/esp,format=raw,if=virtio \
	-device ramfb \
	"${PAYLOAD_DEVICE[@]}" \
	-display none \
	-serial stdio \
	-monitor tcp:127.0.0.1:$MON,server,nowait \
	-no-reboot "$@" 2>&1
