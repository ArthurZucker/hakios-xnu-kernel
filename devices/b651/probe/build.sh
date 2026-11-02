#!/bin/sh
# Build the HakiOS gate-1 probe -- devices/b651/probe/probe.S -> probe.img
#
# Produces three things in this directory:
#   probe.elf   linked ELF (for llvm-objdump / tools/check_image.py)
#   probe.img   flat arm64 Image: 64-byte header at offset 0, then code
#   probe-qemu.img  same source built for QEMU virt (PL011 @ 0x09000000)
#
# Idempotent: every run rewrites all three from source.  Nothing else in the
# repo is touched.  Requires clang + llvm-objcopy (Homebrew LLVM is fine).
set -eu

cd "$(dirname "$0")"

CLANG=${CLANG:-clang}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
TARGET=aarch64-none-elf

# Link at 0x0: text_offset = 0 and the header's `b entry` is a PC-relative
# branch, so the image is position-independent and runs wherever it is loaded.
LDFLAGS="-nostdlib -ffreestanding -fuse-ld=lld -Wl,-Ttext=0x0 -Wl,--image-base=0x0 -Wl,--build-id=none -Wl,-n"

build() {
    name=$1; shift
    echo "[build] $name"
    # shellcheck disable=SC2086
    $CLANG --target=$TARGET $LDFLAGS "$@" \
        -o "$name.elf" probe.S
    $OBJCOPY -O binary "$name.elf" "$name.img"
    size=$(wc -c < "$name.img" | tr -d ' ')
    echo "[build] $name.img  $size bytes"
}

build probe      -DUART_BASE=0x11002000 -DUART_PL011=0
build probe-qemu -DUART_BASE=0x09000000 -DUART_PL011=1

echo
echo "[check] first 64 bytes of probe.img (the arm64 Image header):"
od -An -tx1 -v -N64 probe.img | sed 's/^/    /'
echo
echo "[check] header parse:"
python3 check_image.py probe.img || {
    echo "header check FAILED" >&2
    exit 1
}
echo
echo "[check] disassembly of the MT6877 UART write loop (probe.elf):"
${OBJDUMP:-llvm-objdump} -d --no-show-raw-insn probe.elf \
    | sed -n '/<uart_putc>:/,/^$/p' | sed 's/^/    /'
echo
echo "[done] artifacts: probe.elf probe.img probe-qemu.elf probe-qemu.img"
