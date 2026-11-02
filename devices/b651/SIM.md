# Working on the port without the phone

`make qemu-b651` runs a machine shaped like the HiBreak Pro. The phone stays untouched until something boots here
first. This file says what the simulator is, what it is honestly not, and which questions it can and cannot answer.

```console
make qemu-b651 SECS=60        # boots and stops after 60 s, log in logs/qemu-b651.log
python3 qemu/epd.py --once /tmp/screen.png    # what the e-ink panel would show
```

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
python3 qemu/epd.py --mode du --fps 4        # watch the ghosting accumulate
python3 qemu/epd.py --mode gc16 --once out.png
```

**It is a model, not the controller.** The frame counts, the flash and the residue are plausible numbers chosen to
show the *shape* of the problem — they are not measurements of this panel, and the waveform LUTs they approximate
are in the phone's `waveform` partition, unread. Use it to get the driver's state machine right, not to predict how
fast the real screen refreshes.

`PANEL_W, PANEL_H = 824, 1648` in `epd.py` come from this unit's own screencaps (its `screencap` output is exactly
that), not from the DT — the `/eink` property values were root-only.

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
