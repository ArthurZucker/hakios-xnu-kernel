# HakiOS gate-1 probe — B651 / MT6877

The smallest artifact that answers **"does this phone run foreign unsigned
arm64 code at all?"** (`GATE1.md` step 4). It writes one line to UART0 and
spins forever. It writes nothing else — no flash, no memory, no MMIO except
the UART.

```text
power on → ABL/LK → boot slot kernel (this image)
                         │
                         ▼
  +----------------------------------------------+
  │ 0x00  b entry          <- arm64 Image header │ 64 B
  │ 0x38  "ARM\x64" magic  0x644d5241            │
  +----------------------------------------------+
  │ 0x40  entry:  adrp banner / byte loop        │
  │        uart_putc: poll LSR.THRE -> THR       │
  │        wfe spin forever                      │
  +----------------------------------------------+
                         │
                         ▼
        UART0 @ 0x11002000 115200n1 (16550/8250_mtk)
        "HAKIOS PROBE: arm64 image ran"
```

## What it proves

- ABL/LK handed control to an image that **HakiOS built**, not Bigme's.
- The arm64 `Image` header is accepted (right magic, right entry branch).
- Code executes with the MMU off at the load address ABL chose.
- The shared UART works well enough to print — the same UART every later
  stage depends on (`SCOPE.md` gate 2, `HARDWARE.md`: UART0 `0x11002000`).

That is gate 1. If the banner appears, a HakiOS loader can run here.

## What it does NOT prove

| not proven | why it matters | where it is answered |
|---|---|---|
| that the bootloader is **unlocked** | LK may load and still refuse to branch to an unsigned kernel | `GATE1.md` step 6 — read `seccfg` off flash |
| that `fastboot boot` exists | MTK LK often removes it | `GATE1.md` step 4a |
| DRAM, PMIC, UFS, panel, GIC | probe touches none of them | `GATE1.md` step 2 — pull the real DTB |
| the boot image is *flashable* | it is packed, not validated by LK | gate 1 step 5 backup first |
| UART pinmux / baud on the real board | assumes the OPPO-derived DT (`HARDWARE.md`) | the phone's own DTB |
| that the probe survives `wfe` with no GIC programmed | `wfe` is a hint; a correct core wakes on any event or timeout | hardware only |

A **silent** result is informative but ambiguous: it means "no banner", not
why. The three candidates, in order of likelihood, are a locked bootloader, a
different UART base/baud on the real board, or `fastboot boot` silently
returning to fastboot.

## Files

| file | what |
|---|---|
| `probe.S` | the probe. arm64 asm, freestanding, no libc, no stack, no relocations |
| `build.sh` | builds both variants, dumps the header, parses it, disassembles the UART loop |
| `check_image.py` | stdlib parser for the 64-byte arm64 `Image` header |
| `run-qemu.sh` | QEMU `virt` harness (PL011) — the real test of the control flow |
| `probe.img` | **MT6877 build** — UART `0x11002000`, 16550 layout. The phone artifact |
| `probe-qemu.img` | QEMU build — UART `0x09000000`, PL011. Not for the phone |
| `probe-boot.img` | `probe.img` packed as the kernel of an Android boot image (v0) |

Everything except `probe.S`, `build.sh`, `check_image.py`, `run-qemu.sh` and
this file is generated; `build.sh` is idempotent and rewrites it all.

## Build

```console
$ ./build.sh
[build] probe
[build] probe.img  176 bytes
...
0x00  code0       = 0x14000010        # b entry (offset +64)
0x10  image_size  = 0xb0              # == file size
0x38  magic       = 0x644d5241        # "ARMd"
OK: valid arm64 Image header
```

`text_offset = 0`, `flags = 0` (LE, 4K pages) and an image that is exactly one
section, so `image_size` equals the flat file size. Entry is a real `b`, not a
`br` to an absolute address, so the image is position-independent.

### UART, MT6877 vs QEMU

Same source, two register layouts, chosen at build time:

| | MT6877 (`probe.img`) | QEMU virt (`probe-qemu.img`) |
|---|---|---|
| base | `0x11002000` | `0x09000000` |
| model | 16550 / `mediatek,mt6577-uart` | PL011 |
| status reg | LSR `+0x14`, THRE = bit 5 | FR `+0x18`, TXFE = bit 7 |
| data reg | THR `+0x00` | DR `+0x00` |
| access | 32-bit, stride 4 (`UPIO_MEM32`) | 32-bit |

Both poll with a `0x10000` iteration bound and drop the byte if the UART never
becomes ready, so an unclocked UART cannot wedge the CPU.

## Test without the phone

```console
$ ./run-qemu.sh 3
[qemu] loading probe-qemu.img (PL011 @ 0x09000000), 3s timeout
[qemu] --- serial output begins ---
HAKIOS PROBE: arm64 image ran
PASS: banner came out of QEMU's serial.
```

QEMU's `-kernel` on `virt` is the same handoff as ABL's: build a DTB, put it in
`x0`, branch to the image base. The probe ignores `x0` (it may be 0), which is
also what makes it safe on the phone.

The negative control matters as much as the positive one: the **MT6877 build**
loaded into the same QEMU produces **zero bytes**, because `0x11002000` is not
a UART there. That is what shows the harness is actually exercising the UART
path and not printing from somewhere else.

```console
$ qemu-system-aarch64 -M virt -cpu cortex-a53 -m 256M -display none \
      -monitor none -serial file:/tmp/s -no-reboot -kernel probe.img
$ wc -c < /tmp/s
0
```

Note that QEMU need not exit: the probe spins by design, so the harness kills
it after a wall-clock timeout and reads the serial file.

## Packing for the phone

`mkbootimg.py` header v0 (MTK LK-era), no ramdisk, kernel load address
`0x40080000` (`base 0x40000000 + kernel_offset 0x80000` — the arm64
convention; adjust to whatever the phone's own boot image uses):

```console
$ python3 ../tools/mkbootimg.py --kernel probe.img --no-ramdisk \
      --header-version 0 --page-size 4096 \
      --base 0x40000000 --kernel-offset 0x80000 --board b651 \
      --cmdline "console=ttyS0,921600n1" -o probe-boot.img
$ python3 ../tools/mkbootimg.py --info probe-boot.img
kernel   176 B   0x40080000   1 pages   Linux arm64 Image (ARM\x64)
```

`mkbootimg.py`'s own `--info` recognises the magic, and the kernel section in
the packed image is byte-identical to `probe.img` (sha256 match).

⚠ **Before using `0x40080000`:** take it from a dump of *this phone's* stock
`boot_a` (`mkbootimg.py --info boot_a.img`), not from this default. A wrong
load address is a no-banner result indistinguishable from a locked
bootloader.

## The phone-side command — documented, NOT run

Nothing below has been executed. The probe was never connected to the phone.
Order matters: step 5 (backup) comes before anything that writes.

```console
# RAM only, writes nothing -- try this first (GATE1.md step 4a)
adb reboot bootloader
fastboot boot probe-boot.img

# if LK has no `fastboot boot`, it writes boot_a -- only after the
# GATE1.md step-5 backup of boot_a/boot_b/lk_a/preloader_a/seccfg/nvdata
fastboot flash boot probe-boot.img
fastboot reboot
```

Watch UART0 at **115200 8N1** for `HAKIOS PROBE: arm64 image ran`. If the
board's console runs at the DT's `921600n1`, increase the spin bound in
`probe.S` or expect mangled bytes — a wrong baud shows garbage, not silence,
which is itself a useful signal.

## Design notes

- **No stack use in the print loop.** `bl`/`ret` to a leaf that saves nothing
  is safe; the probe never assumes SP is valid, because ABL may not set one.
- **`x0` is read but not required.** No DTB parsing, so a null or garbage
  pointer cannot fault before the banner.
- **Position independence.** `adrp`/`add` only; no absolute pointers in code,
  no relocations (`--build-id=none`, `-n`).
- **Header and code share one `.text`.** A separate `.text.header` is treated
  as an orphan section by lld and lands *after* `.text`; a single section
  preserves source order and makes `image_size` exact.
- **`wfe` spin, not a halt.** There is no reset vector to return to and no
  PSCI setup, so spinning is the only honest option.
