# Working on the port without the phone

`make qemu-b651` runs a machine shaped like the HiBreak Pro. The phone stays untouched until something boots here
first. This file says what the simulator is, what it is honestly not, and which questions it can and cannot answer.

```console
make qemu-b651 SECS=60        # boots and stops after 60 s, log in logs/qemu-b651.log
python3 qemu/epd.py --once /tmp/screen.png    # what the e-ink panel would show
```

## It boots (2026-11-02)

```text
XNULOADER v3   CurrentEL=1 MIDR=414fd0b1 CNTFRQ=62500000
kernel.ver=2.1.13-hakios-darwin22.5-mt6877-...
platform qemu-virt
display 1125 x 2436 at 13b9fb000
cores for the kernel, in the order it takes them: 7 6 5 4 3 2 1
kernel loaded at pa 4b004000..4c2c0000
Darwin Kernel Version 22.5.0 ... DEVELOPMENT_ARM64_BCM2837
MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1125x2436
Hello from user space on Darwin!
kernel.ver=2.1.13-hakios-darwin22.5-mt6877-...
```

The `hello` program read the kernel version back through `sysctl` and printed it, which is the same
end-to-end check upstream's `qemu-open` makes. After that XNU panics with `initproc failed to start`
— the init program exited, and XNU treats that as fatal. **That panic is the open `hello` payload's own
behaviour, not the b651 machine's**: it is what this configuration does, and it is why the payload is a
test, not a system.

### Two host bugs this found, both fixed here and not upstream

| bug | effect |
|---|---|
| `xnu/apply.sh` appends to `.git/info/exclude` | git ≥ 2.54 no longer creates `.git/info/` on `init`, so the kernel build died before compiling anything |
| `qemu/run.sh` uses `cp -u` | GNU-only; BSD `cp` has no `-u`, so upstream's own QEMU target cannot run on a Mac |

`run-b651.sh` avoids both and builds its own NVRAM, since Homebrew ships `edk2-aarch64-code.fd` without
an aarch64 vars template.

## The machine, next to the phone

| | HiBreak Pro | `qemu-b651` | same? |
|---|---|---|---|
| CPU | 2× Cortex-A76 + 6× Cortex-A55 | `-cpu cortex-a76`, 8 vCPU | shape yes, hetero no |
| GIC / timer | GICv3, `armv8-timer` @13 MHz | GICv3, `armv8-timer` | ✓ |
| RAM | 8 GB | 4 GB (configurable) | no |
| Firmware | MediaTek preloader → LK → … | edk2 (AAVMF) → loader | different path |
| Console | UART0 `0x11002000`, 921600n1 | `-serial stdio` | ✓ enough |
| Display | e-ink, Bigme EPD + waveforms | ramfb + `qemu/epd.py` model | model only |
| Storage | UFS `0x11270000` (MediaTek ufs-hc) | virtio-blk | **no** |
| USB | MediaTek MTU3 device mode | none | **no** |
| PMIC / thermal / cpufreq | pwrap / PMIF / i2c5, therm_ctrl, MCUPM | absent | **no** |
| GPU | Mali (valhall) | none (software draw) | **no** |

### What that means, plainly

**The simulator can answer:** does the kernel build for this core; does the loader hand off; do the *generic* ARM64
paths work; does a driver's own logic behave (the EPD state machine, a filesystem, a protocol decoder); does the
e-ink refresh look wrong before it looks wrong on the phone.

**It cannot answer:** anything that touches a MediaTek block. A driver written against pwrap, PMIF, MTU3, MTK UFS
or the Mali DVFS table will not bind here — there is no register to poke. Those have to be written against
`HARDWARE.md` and tested on the phone, and there is no way around that.

So the split is: **logic here, registers there.**

## The e-ink model

`qemu/epd.py` reads the framebuffer over QEMU's monitor and applies what a panel would actually do:

| mode | frames | flashes | residue | what it is |
|---|---|---|---|---|
| `gc16` | 24 | yes | 0% | full grey-level clear — slow, flashes black-white, leaves nothing |
| `gl16` | 16 | no | 6% | grey-level without the flash — this is what `eink_glx16_disable` in the DT refers to |
| `du` | 6 | no | 22% | direct update — fast, leaves a shadow of the old content |
| `a2` | 2 | no | 45% | animation — fastest and dirtiest |

```console
python3 qemu/epd.py --burst /tmp/frames            # follow a whole boot, one PNG per frame
python3 qemu/epd.py --burst /tmp/frames --mode auto  # what a controller would actually do
python3 qemu/epd.py --mode du --fps 4              # watch the ghosting accumulate
python3 qemu/epd.py --once out.png
```

**Use `--burst` on a boot.** A guest lives about eight seconds here, and `--once` almost always misses the window
between the loader handing over and XNU drawing. `--burst` follows the boot to the end and writes `fNN.png` per frame.

### `--mode auto`: damage rects and a waveform per update

A real EPDC is not given "here is a frame". It is given *this rect, this waveform, tell me when it is done* — and it
does nothing at all when the picture has not changed. `--mode auto` models that: it diffs the frame against the one the
panel was last driven with, and

| the change | what the model does |
|---|---|
| none | **no update** — the panel is not addressed |
| under 5% of the panel | `a2`, on the damage rect only |
| 5–30% | `du`, on the damage rect only |
| over 30%, or 12 partial updates deep | `gc16`, whole panel — large change, or the ghosting has caught up |

Outside the rect the panel keeps exactly the frame it had, because those pixels were never driven. A boot through it:

```text
[  0.6s] f01 800x600 du   rect=0,0-800,600          ink= 2.4% partial=1
[  1.3s] f02 800x600 a2   rect=0,576-159,588        ink= 2.8% partial=2
[  4.1s] f06 800x600 a2   rect=0,576-639,588        ink= 3.8% partial=6
[  5.5s] f08 1125x2436 gc16 rect=0,0-1125,2436      ink= 4.6% partial=0
8 frames, 54.7s: the guest ended
```

95 samples of the framebuffer produced **8 updates**. UEFI scrolling a line is a `a2` on that line's rect; the loader
changing the mode is a `gc16` of the whole panel. That ratio is the whole reason e-ink is usable at all, and it is the
number to watch when the real driver is written.

### What it found on the first real boot

XNU's own video console draws to the panel, so the model has genuine content to work with:

```text
[  0.6s] f01 800x600  colours= 65 ink= 98.5%     UEFI's own screen
[  5.8s] f07 1125x2436 colours=  1 ink=100.0%     the loader switched the mode: blank panel
[  6.6s] f08 1125x2436 colours=  2 ink=100.0%     XNU's console, drawn          (after the crop fix below)
```

Two things this exposed that were not obvious from reading the code:

1. **The mode changes mid-boot.** UEFI runs at 800×600; the loader takes the panel to 1125×2436 and XNU draws there.
   The first version of the model blended the two sizes together and crashed — a real driver has to treat a mode
   change as a new scan, not as a frame.
2. **The panel is 1125×2436, not the 824×1648 `screencap` reports.** The unit's own `screencap` comes out at 824×1648,
   which is a different logical mode; cropping captures to it sliced the right edge off every console line. The
   loader's mode (`display 1125 x 2436`) is the panel: `PANEL_W, PANEL_H = 1125, 2436`.
3. **`initialize_screen: ... d=00000000` is not a fault.** The depth reads zero in that log line because XNU prints
   `v_type`, which it hardcodes to 0 on ARM (`video_console.c:2819`); the depth itself arrives intact. Chasing it as
   a bug costs an hour.

### Getting the console onto the panel at all

`loader.c:3102` sets `v_display = verbose ? 0 : 1`, and XNU keeps text off the screen when that is 1. So the boot
console only appears with **`-v`** in the payload's cmdline:

```console
xnuloader/mkpayload.py -o qemu/payload.bin kernel=hakios-kernel ramdisk=userland/hello --cmdline "-v cpus=8 bootcpu=7"
```

The loader's own comment explains why it is off by default: on a phone-sized screen the verbose console scrolls
megabytes per line and slows boot to a crawl. That is exactly the e-ink problem in miniature, and a reason the model
is worth having.


**It is a model, not the controller.** The frame counts, the flash and the residue are plausible numbers chosen to
show the *shape* of the problem — they are not measurements of this panel, and the waveform LUTs they approximate
are in the phone's `waveform` partition, unread. Use it to get the driver's state machine right, not to predict how
fast the real screen refreshes.

### `/dev/hakios-epd` runs here (2026-11-02)

The panel driver is not a skeleton any more for the simulator: `epd=1 epd_model=1` attaches its **model backend**,
and `qemu_defconfig` builds it in. An update is timed by the waveform it names and then completes through a
`thread_call` — the same place the controller's interrupt will complete it on the phone.

```console
# in the payload cmdline, then run devices/b651/tests/epd_test.c as the RAM disk
epd=1 epd_model=1
```

```text
hakios-epd: /dev/hakios-epd; 20 C, powerdown after -1 ms
hakios-epd: no EPDC (this is the simulator ...), so the panel is MODELED
hakios-epd: update 101: 32x32 at 0,0, a2 (33 ms)
hakios-epd: update 102: 32x32 at 0,0, gc16 (400 ms), whole panel
epd_test: all 12 checks pass
```

What that exercises, and what it does not:

| | exercised here | only on the phone |
|---|---|---|
| the ABI (ioctl numbers, struct, rect check) | ✓ | |
| one update at a time, `EBUSY` on a marker in flight | ✓ | |
| `HAKIOS_EPD_WAIT` sleeping, and waking on completion | ✓ | |
| `EINVAL` for a bad rect / marker / mode / waveform | ✓ | |
| registers, LUTs, the interrupt, the PMIC, VCOM | | all of it |

`epd_model` is a boot argument and not a guess at the machine. Inside the kernel, "this machine has no EPDC" and
"the phone's dtbo has not been read" are the same observation — both are a `base` of 0 — so the caller says which
backend it wants. A phone boot never passes it, and `epd=1` alone there still reaches `epd_attach()`'s `ENOTSUP`.

The device is made from `bsd_init` beside `/dev/hakios-touch`, not from `STARTUP`: a node made before
`devfs_kernel_mount` is not in `/dev`, and every open of it fails `EBADF`. That is not obvious from the driver code
and it cost a boot to find.

`PANEL_W, PANEL_H = 1125, 2436` in `epd.py` come from the mode the loader sets and XNU draws at, not from the DT —
the `/eink` property values are root-only, and the unit's `screencap` reports a different logical mode (824×1648).

## Rules of engagement

`SCOPE.md` gate 1 is still open (`sys.oem_unlock_allowed=0`, bootloader locked), so the phone cannot take an
unsigned image today regardless. That is a good reason to work here, and it does not make the phone safe:

- **Nothing in `devices/b651/` is a phone-writing path.** No script in this port flashes anything. If one ever
  needs to, it says so in its first line and in `GATE1.md`.
- **The phone is read-only** until something boots in QEMU. `adb` reads are fine; `fastboot` and `mtkclient` are
  not, until there is a payload worth risking.
- **When the simulator boots end to end**, that is the moment the phone becomes the next test — not before.

## What is still needed to make the simulator more truthful

| gap | what would close it | cost |
|---|---|---|
| real panel geometry and `/eink` timings | read the dtbo, or root | BROM access |
| real waveform LUTs | read the `waveform` partition | BROM access |
| MTK UFS instead of virtio-blk | write the driver against `HARDWARE.md` | weeks |
| USB device mode | port MTU3 into Mu-Silicium | weeks |
| hetero big.LITTLE | QEMU cannot model A76+A55 asymmetry on `virt` | not possible |
