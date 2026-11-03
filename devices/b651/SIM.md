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
kernel.ver=2.1.13-hakios-darwin22.5-gefa434b9
platform qemu-virt
display 1125 x 2436 at 13b9fb000
cores for the kernel, in the order it takes them: 0 1 2 3 4 5 6 7
kernel loaded at pa 4b004000..4c2c0000
Darwin Kernel Version 22.5.0 ... DEVELOPMENT_ARM64_BCM2837
MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1125x2436 (4500 bytes per row)
Hello from user space on Darwin!
kernel.ver=2.1.13-hakios-darwin22.5-gefa434b9
hello: no clock here, staying anyway
qemu-system-aarch64: terminating on signal 15
```

(`logs/qemu-b651.log`, a plain `make qemu-b651` with the default `ARGS_OPEN` — `cpus=1`. The version string carries
this tree's git suffix, so it moves with every commit. The core line lists the cores the kernel was told to take, in
the order the loader takes them — `loader.c:3407-3432`, nearest to the boot core first — which is why a
`-v cpus=8 bootcpu=7` boot prints `7 6 5 4 3 2 1` (`logs/re-epd.log:38`) and this one prints all eight in index
order. It is not a boot-core ordering.)

The `hello` program read the kernel version back through `sysctl` and printed it, which is the same
end-to-end check upstream's `qemu-open` makes.

**It then stays resident, and the run ends on the timeout instead of a panic.** `hello` only exits when it was
given `hello_wait=N` and slept the whole N (`userland/src/hello.c:202-204`); with no `hello_wait` — which is what
`ARGS_OPEN` gives it, `cpus=1` and nothing else — it says `hello: no clock here, staying anyway` and sleeps in a loop
(`:205-209`), so QEMU is stopped by `run-b651.sh`'s own `timeout`. A panic of the form
`initproc failed to start -- no exit reason available` is what an **exiting** init does to XNU. `logs/boot-v.log`
has one at `:148`, from a `hello` that exited unconditionally — its cmdline (`:40`) has no `hello_wait`, and
`:142` says `pid 1 exited`. That was the payload's own behaviour, not a fault of the b651 machine, and it is why
`hello_wait` exists. A boot that ends at the timeout with `qemu-system-aarch64: terminating on signal 15` and no
panic buffer (`logs/re-epd.log:163`, `logs/qemu-b651.log:137`) is the healthy one. The payload is still a test, not a
system — but "it panics every boot" is no longer true, and `run-b651.sh`'s `QEMU_HOLD` is for the boots where it does.

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
| Display | e-ink, Bigme EPD + waveforms | ramfb, a virtio GPU, and `qemu/epd.py` as the panel model | model only |
| Storage | UFS `0x11270000` (MediaTek ufs-hc) | virtio-blk | **no** |
| USB | MediaTek MTU3 device mode | none | **no** |
| PMIC / thermal / cpufreq | pwrap / PMIF / i2c5, therm_ctrl, MCUPM | absent | **no** |
| GPU | Mali-G68 (valhall) | virtio GPU: 2D scanout always, virgl 3D with `QEMU_GL=1` | **no Mali, but a real GL path on the host** |

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
| `gc16` | 24 | yes | 0% | full grey-level clear — slow, and it clears the ghosting; the model returns the clean picture, it does not draw the intermediate flash |
| `gl16` | 16 | no | 6% | grey-level without the flash — this is what `eink_glx16_disable` in the DT refers to |
| `du` | 6 | no | 22% | direct update — fast, leaves a shadow of the old content |
| `a2` | 2 | no | 45% | animation — fastest and dirtiest |

`flashes` is a property of the waveform, not a rendered frame: `settle()` returns the *settled* picture for a
flashing mode (`qemu/epd.py:333-337`), so a `gc16` capture is the clean page and not the black-then-white it goes
through on the way there. If you need the blink itself, that is a driver-visible timing question the model does not
answer — the `<mode>.flash` knobs (`KNOBS`, `qemu/epd.py:78-97`) are in `panel.json` and `--calibrate` prints them.

```console
python3 qemu/epd.py --burst /tmp/frames              # follow a whole boot, one PNG per *update*
python3 qemu/epd.py --burst /tmp/frames --mode auto  # what a controller would actually do
python3 qemu/epd.py --mode du --fps 4                # watch the ghosting accumulate
python3 qemu/epd.py --once out.png
python3 qemu/epd.py --demo                           # the model's self-check: no QEMU, no files
```

**Use `--burst` on a boot.** The guest now stays resident until `run-b651.sh`'s `timeout` stops it, so a boot is
however long you asked `SECS` for — the captures in `logs/` are 54.7 s (`--burst`, 8 updates), 59.8 s (63 updates)
and 69.4 s (95 updates). `--once` still almost always misses the window between the loader handing over and XNU
drawing, because that is one moment in a longer run. `--burst` follows the boot to the end and writes `fNN.png` per
**update**; samples in which nothing moved are not written and not printed.

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
[  0.6s] f01 800x600 du   rect=0,0-800,600            colours= 65 bg=  0,  0,  0 ink=  2.4% partial=1
[  1.3s] f02 800x600 a2   rect=0,576-159,588          colours= 66 bg=  0,  0,  0 ink=  2.8% partial=2
[  4.1s] f06 800x600 a2   rect=0,576-639,588          colours= 66 bg=  0,  0,  0 ink=  3.8% partial=6
[  5.5s] f08 1125x2436 gc16 rect=0,0-1125,2436        colours=  2 bg=  0,  0,  0 ink=  4.6% partial=0
8 frames, 54.7s: the guest ended
```

(`logs/epd-final.log`. The first frame's rect is the whole 800×600 mode, which is only 17.5% **of the panel**, so
`auto_mode` picks `du`, not `gc16` — the thresholds are shares of `PANEL_W*PANEL_H`, not of the current mode
(`qemu/epd.py:368-376`).)

`--burst` samples the framebuffer at `panel.fps` (2 by default) and writes a frame only for a sample that changed, so
**the number of lines in the log is the number of updates**, not the number of samples: `8 frames, 54.7s` is 8
updates out of roughly 110 samples (54.7 s × 2 fps; the log does not print the sample count, so that 110 is arithmetic,
not a measurement), and that ratio is the whole reason e-ink is usable at all. UEFI scrolling a line is an `a2`
on that line's rect; the loader changing the mode is a `gc16` of the whole panel.

One caveat on `logs/epd-auto.log`, which looks like the best cross-check and is not: its first 7 lines carry damage
rects (800×600, `auto`) and the remaining 88 are `gc16 whole panel`, which is what a *fixed* `--mode gc16` prints. It
is two runs' output in one file, and the `95 frames` at the end belongs to the second. Do not quote it as
"95 samples → 95 updates" or as a single auto-mode capture.

### What it found on the first real boot

XNU's own video console draws to the panel, so the model has genuine content to work with. These are the first
boot's numbers, from **the epd.py of 2026-11-02**, before the panel was quantised and before the console polarity
was flipped — keep them for what they teach, not as output you will see again today:

```text
[  0.6s] f01 800x600  colours= 65 ink= 98.5%     UEFI's own screen
[  5.8s] f07 1125x2436 colours=  1 ink=100.0%     the loader switched the mode: blank panel
[  6.6s] f08 1125x2436 colours=  2 ink=100.0%     XNU's console, drawn          (after the crop fix below)
```

Why those numbers cannot be reproduced today, and what to expect instead:

- `colours= 65` is the **unquantised** 800×600 UEFI screen: 65 raw framebuffer greys. `panel()` now snaps every
  pixel to the waveform's level count (`qemu/epd.py:285-305`), so a 16-level `gc16` frame can hold at most 16
  distinct colours and a two-state `du`/`a2` frame at most 2. Quantisation landed in `7f274bc` (2026-11-03 07:33)
  and the two-state `MODE_LEVELS` in `16c56d9` (08:05); every log here is from 2026-11-02, so **none of them was
  produced by the `epd.py` in the tree** and their `colours=`/`ink=` are not comparable with a fresh capture. (The
  `colours= 2` on the 1125×2436 frames of `logs/epd-final.log` is not evidence against that — it was already two
  colours, because the console draws pure black and pure white and nothing between.)
- `ink= 98.5%` came from the pre-`describe()` metric, which counted every pixel that was not `(255,255,255)` as ink.
  On a white-ink console that made a **black** page 98.5% ink and a blank one 0%. `describe()` now takes the modal
  colour as the background (`qemu/epd.py:432-446`), so ink is "not the page colour" in either polarity. Checked
  directly on one page of text and its exact inversion: both report `ink= 8.5%`, only `bg=` differs (`0,0,0` vs
  `255,255,255`). Settled through the panel model it stays `ink= 8.5%` and only `bg=` moves again (`34,34,34` for the
  black page, `221,221,221` for paper) — so the number means the same thing in all four states. A blank panel is
  `ink=  0.0%` in either polarity.

Three things this exposed that were not obvious from reading the code:

1. **The mode changes mid-boot.** UEFI runs at 800×600; the loader takes the panel to 1125×2436 and XNU draws there.
   The first version of the model blended the two sizes together and crashed — a real driver has to treat a mode
   change as a new scan, not as a frame.
2. **The panel is 1125×2436, not the 824×1648 `screencap` reports.** The unit's own `screencap` comes out at 824×1648,
   which is a different logical mode; cropping captures to it sliced the right edge off every console line. The
   loader's mode (`display 1125 x 2436`) is the panel: `PANEL_W, PANEL_H = 1125, 2436`.
3. **`initialize_screen: ... d=00000000` is not a fault.** The depth reads zero in that log line because XNU prints
   `v_type`, and `v_type = 0` is hardcoded for every non-i386/ARM target (`video_console.c:2827-2831`, the `#else`
   arm of the `#if defined(__i386__) || defined(__x86_64__)`); the depth itself arrives intact
   (`logs/boot-burst.log:229`, `w=00000465, h=00000984` = 1125×2436). Chasing it as a bug costs an hour.

### Getting the console onto the panel at all

`loader.c:3125` sets `v_display = verbose ? 0 : 1`, and XNU leaves the console in graphics mode when that is 1
(`pexpert/arm/pe_init.c:514-518`), which is what keeps its text off the screen. So the boot console only appears
with **`-v`** in the payload's cmdline:

```console
xnuloader/mkpayload.py -o qemu/payload.bin kernel=hakios-kernel ramdisk=userland/hello --cmdline "-v cpus=8 bootcpu=7"
```

or, for the runs the Makefile already defines, `make qemu-b651-epd` and friends, whose `--cmdline` starts with `-v`.
The loader's own comment explains why it is off by default: on a phone-sized screen the verbose console scrolls
10 MB per line and slows boot to a crawl (`loader.c:3152-3156`). That is exactly the e-ink problem in miniature, and
a reason the model is worth having.

### Console polarity

**The console draws dark ink on a paper-white page.** This is a deliberate change in the kernel, not a property of the
panel model, and it is the right one for a reflective display:

```c
/* osfmk/console/video_console.c:186-193 */
#define COLOR_BACKGROUND 7        /* was 0 (black) in stock XNU */
#define COLOR_FOREGROUND 0        /* was 7 (white) in stock XNU */
```

Stock XNU's console is white on black, which is what a CRT or an LCD wants: the dominant state is the backlight's
absence, and text is lit pixels. On an electrophoretic panel there is no backlight, and the state the panel *holds
without being driven* is paper white. A black page is wrong here for three separate reasons:

| the black page | why it is wrong here |
|---|---|
| the page is a dark field the panel has to keep holding | paper white is the state pigment settles to; a screen that is black everywhere is one the controller is continuously holding away from rest, and it is the state that vanishes (turns white) when the panel loses drive |
| every full refresh is a visible inversion | `gc16` clears by driving the whole panel through the opposite state, so a black page refreshes as a black→white→black sweep across the entire screen instead of a brief blink on a page that already looks like paper |
| the ink is inverted relative to the display's whole purpose | reflective ink *is* dark. Dark glyphs on paper white is the state the panel reproduces with the fewest driven pixels and the smallest contrast excursion per update |

On the last point, be precise about the model rather than the folklore: a scrolling line is a line-sized damage rect
either way, so `auto_mode` picks `a2` for it in **both** polarities (`qemu/epd.py:368-376` — the thresholds are shares
of the panel, and a line is far under 5%). The saving is not in the waveform choice per update; it is that the
*settled* state is the one the panel can hold, so a partial update does not have to re-establish a full-screen dark
field, and a `gc16` is not an inversion of the whole page.

So the swap is the whole difference between "the simulator draws a terminal" and "the simulator draws a page". The
glyphs are the same pixels either way — only the two colours trade places.

What the model does with that, and what it does **not** assume:

- `describe()` no longer hardcodes "background is white". It takes the **modal** colour as the background and counts
  everything else as ink (`qemu/epd.py:432-446`), so `ink=` means the same thing before and after the swap, and would
  survive a swap back. Verified on one page of text: white-on-black and its exact inversion both report `ink= 8.5%`,
  only `bg=` differs (`0,0,0` against `255,255,255`).
- `settle()`'s residue takes the **darker** of the new and old pixel (`ImageChops.darker`, `qemu/epd.py:339-346`)
  because dark *is* ink on this panel. That is a property of the ink, not of the console, and the code comment says so
  — which is why the swap did not need a change there.
- `epd_live.py` borrows that same function (`DESCRIBE = getattr(epd, "describe", None)`, `epd_live.py:59-60`) and
  `epd_view.py` has its own `ink()` that takes the modal colour for the same reason (`epd_view.py:136-145`). Its
  docstring still says "XNU's console is white on black", which was true when it was written; the code is
  polarity-agnostic either way.

Every trace in `logs/` predates the swap — all of them report `bg=  0,  0,  0`, i.e. a black page. **There is no
captured boot with the dark-on-paper console yet**, so the model side of this section is verified (the `ink=` and
`settle()` behaviour below is checked directly) and the kernel side is read from the source, not from a run. If you
are comparing an ink percentage against a log, check its `bg=` first.

**Caveat, and it is the one that will bite.** The swap lives in `xnu-src/xnu-8796.121.2/osfmk/console/video_console.c`,
which `.gitignore`'s whitelist excludes from the repository: `xnu/apply.sh` recreates that tree from
`xnu/hakios.patch` plus the files under `xnu/files/`. `grep COLOR_BACKGROUND xnu/hakios.patch` finds nothing, so
the two lines are **not** in anything `apply.sh` applies. Run `xnu/apply.sh` and the console goes back to stock
white-on-black. This is documenting the working tree, not a reproducible build — see the gap table at the end.

**It is a model, not the controller.** The frame counts, the flash and the residue are plausible numbers chosen to
show the *shape* of the problem — they are not measurements of this panel, and the waveform LUTs they approximate
are in the phone's `waveform` partition, unread. Use it to get the driver's state machine right, not to predict how
fast the real screen refreshes.

Three things it does model because they change what you see, not just how long it takes:

| what | how | evidence |
|---|---|---|
| **grey levels** | every frame is snapped to the waveform's own level count and mapped into `INK_BLACK..PAPER_WHITE` (0x22..0xDD = 34..221), so a full black is 34 and a full white 221, as on a panel. `gc16`/`gl16` get `GREY_LEVELS` (16); `du`/`a2` get **2**, the black/white modes (`MODE_LEVELS`, `qemu/epd.py:63-66`) | **assumed** — 4-bit is the common e-ink depth for the grey modes; this panel's is in the unread `waveform` partition. The two-state `du`/`a2` is cited from `mxc_epdc_fb_test.c:68` ("fast black/white animation") |
| **ghosting is per pixel** | residue scales with how far that pixel actually moved, not one flat alpha over the panel: a full black-to-white swing ghosts at the mode's full rate, a small change barely at all | **assumed** in magnitude, **correct** in shape |
| **contrast** | `INK_BLACK`/`PAPER_WHITE` are one knob; change them when a photometer says what the real states are | **assumed** |

Driving a clean page (paper over ink) through each waveform shows the cost directly, in the model's own levels.
`history` is the number of partial updates since the last full clear, which is what `settle()` scales the residue by
(`qemu/epd.py:344-346`):

```text
        history  0     1     2     4     6     12
  gc16          221   221   221   221   221   221    the grey-level clear: back to paper, always
  gl16          209   209   209   196   184   146    no flash, a little grey left, and it darkens as it goes
  du            221   221    34    34    34    34    two states: paper, then ink once the residue has built up
  a2            221   221    34    34    34    34    animation: same two states, sooner (at history 2 vs 4)
```

Two things to read off it, and one to beware of:

- `du` and `a2` are **two-state** waveforms (`MODE_LEVELS`, `qemu/epd.py:66`), so they never show a partial grey:
  the moment the old content is not fully cleared they land on ink black (34), not on a mid grey. That is why they
  are right for text and an animation and wrong for a photograph.
- `gl16` is the only waveform here that fades gradually, which is what "grey levels without the flash" means.
- An earlier version of this table said `du -> 184` and `a2 -> 134`. Those came from the flat-alpha residue model,
  where a whole panel blended toward the old frame by one fraction; `settle()` now scales per pixel by how far that
  pixel actually moved (`qemu/epd.py:338-346`), and a two-state waveform then cannot produce 184 or 134 at all. Any
  number in this file that is not reproducible with `python3 qemu/epd.py --once` / `--burst` is stale.

`python3 qemu/epd.py --once out.png` and `--burst DIR` apply all of this. `qemu/epd_live.py` is the same model over a
live QEMU, in a phone-shaped window.

### `/dev/hakios-epd` runs here (2026-11-02)

The panel driver is not a skeleton any more for the simulator: `epd=1 epd_model=1` attaches its **model backend**,
and `devices/qemu/configs/qemu_defconfig` builds it in explicitly (`CONFIG_HAKIOS_EPD=y`, `CONFIG_EPD=y`,
`CONFIG_EPD_MODEL=y`). An update is timed by the waveform it names and then completes through a `thread_call` — the
same place the controller's interrupt will complete it on the phone. `make qemu-b651-epd` is the whole thing: it
builds `devices/b651/tests/epd_test.c`, packs it as the RAM disk with `-v cpus=8 bootcpu=7 epd=1 epd_model=1`, and
runs it.

```text
hakios-epd: /dev/hakios-epd; 20 C, powerdown after -1 ms
hakios-epd: no EPDC (the simulator has none), so the panel is MODELED: 1125x2436, the epd.py waveform table (gc16 24, gl16 16, du 6, a2 2 frames at 16 ms). The marker, EBUSY and the wait above it are the phone's
hakios-epd: update 101: 32x32 at 0,0, a2 (33 ms)
hakios-epd: update 102: 32x32 at 0,0, gc16 (400 ms), whole panel
hakios-epd: update 106: 1125x2436 at 0,0, gc16 (400 ms), whole panel
epd_test: all 12 checks pass
```

(`logs/re-epd.log:143` (update 101), `:147`, `:156`, `:159`; the first two lines are `:134` and `:135`. The `MODELED`
sentence is one `printf` (`hakios.epd.c:271-280`, applied as `bsd/dev/hakios.epd.c:287-293`) that wraps in the log. **Not reproducible from the log as
written:** the copy of the driver that produced `logs/re-epd.log` printed a shorter sentence (`an update takes the
waveform's time and then completes`) than the driver in the tree prints now (panel size plus the four frame counts).
The line above is the *current* driver's output, so a fresh `make qemu-b651-epd` should print it and the old log will
not — that difference is why the frame counts are quoted here rather than read off a log. The 33 ms and 400 ms are
`frames × EPD_MODEL_FRAME_MS`, `EPD_MODEL_FRAME_MS = 1000/60` (`hakios.epd.c:218`).)

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

## The display through a GPU device, not a memcpy

`virtio_gpu=1` puts the console on QEMU's **virtio GPU** instead of the loader's bare framebuffer
(`drivers/gpu/hakios.virtio_gpu.c`): a 2D resource of the panel's size backed by the console's framebuffer,
`SET_SCANOUT` on display 0, then a transfer-to-host and flush on a 1 Hz `thread_call`
(`clock_interval_to_deadline(1000, NSEC_PER_MSEC)`, `hakios.virtio_gpu.c:855-856`).

The transfer is **not always the whole panel**: the driver keeps a shadow copy of what the device was last given,
compares bands of `GPU_BAND_ROWS` (64) rows of the framebuffer against it, and transfers only the span of bands that
differ (`gpu_damage_span`, `hakios.virtio_gpu.c:700`; `GPU_BAND_ROWS` at `:628`). The same rect is published on
`/dev/hakios-gpu` as `HAKIOS_VIRTIO_GPU_DAMAGE` (`hakios.virtio_gpu.c:83`). With no shadow — the first upload, or a
framebuffer whose pitch is not the resource's stride — the whole panel goes, which is why the first frame below is
the whole thing:

```text
gpu: cmd 0x100 -> reply 0x1101      GET_DISPLAY_INFO
gpu: cmd 0x101 -> reply 0x1100      RESOURCE_CREATE_2D   (1125x2436, B8G8R8A8)
gpu: cmd 0x106 -> reply 0x1100      RESOURCE_ATTACH_BACKING
gpu: cmd 0x103 -> reply 0x1100      SET_SCANOUT
gpu: cmd 0x105 / 0x104 -> 0x1100    TRANSFER_TO_HOST_2D + RESOURCE_FLUSH, once a second
```

It is live, not a still: sampling the host's display over one boot gives **7 distinct images in 63 frames**
(`logs/gpu-burst.log`), tracking the console as it scrolls. What the device shows is what XNU draws.

### A 3D context, made by the guest (2026-11-02)

With `-device virtio-gpu-gl-device` the driver does more than scan out: it negotiates `VIRTIO_GPU_F_VIRGL` and
sends `VIRTIO_GPU_CMD_CTX_CREATE` asking for a virgl2 context. The device answers by having the host's
virglrenderer make a real GL context, so an OK means a GL context exists on the other end of the device:

```text
gpu: virtio GPU in slot 31: a 1125x2436 2D resource on scanout 0, driven from the console's framebuffer
gpu: 3D: a virgl2 context on ctx 0 -- the device made a GL context for it on the host
```

On a device without virgl the same driver says so and keeps the 2D scanout:

```text
gpu: this device has no virgl (no VIRTIO_GPU_F_VIRGL), so there is no 3D half to use
```

**It renders, and the render is on the screen.** The driver takes the whole path: a virgl2 context, a 3D resource
with the console framebuffer attached as its backing, a `CLEAR_TEXTURE` the host's virglrenderer executes as
`glClearTexSubImage`, a `TRANSFER_FROM_HOST_3D` that reads the result back into that framebuffer, and the 2D
scanout that already worked to show it.

```text
gpu: 3D: a virgl2 context on ctx 1 -- the device made a GL context for it on the host
gpu: 3D: resource 2 drawn by the host's GL renderer, read back into the framebuffer, on scanout 0
$ python3 -c "…centre pixel…"      ->  (26, 115, 217)
```

That is the colour the guest asked the GPU to clear to (`0.10, 0.45, 0.85`), on the panel, and **nothing in the
guest rasterised it** — the host's GL did. The readback route exists because a 3D *scanout* binds a texture that
only the display's GL context can see, and on this host the renderer and the display are separate contexts on
separate threads.

Why it stayed invisible for so long, all silent because QEMU ignores virgl's return codes:

| bug | effect |
|---|---|
| `ctx_id = 0` | `virgl_renderer_context_create_with_flags` refuses id 0 outright, so no context existed |
| `SUBMIT_3D` `size` one dword short | vrend parsed a truncated stream |
| no `CTX_ATTACH_RESOURCE` | `CLEAR_TEXTURE` looks the resource up in the context and found nothing |
| the clear colour sent as four floats | it is raw BGRA bytes — `0xcdcccc3d` put the bits of `0.1f` on the screen |
| a trace-stripping edit that deleted `virgl_renderer_submit_cmd` | the command never reached vrend at all |

[`qemu/virgl-macos.md`](../../qemu/virgl-macos.md) is how the device came to work on this host at all.

### A program can draw (`/dev/hakios-fb`)

Until this existed the only thing that could put pixels on the screen was the kernel. `fbdev=1` adds
`devices/b651/drivers/display/hakios.fb.c` (it lands in the kernel tree as `bsd/dev/hakios.fb.c`): the panel as a
character device, with its geometry (`HAKIOS_FB_INFO`), a blit (`HAKIOS_FB_BLIT` names the rect, the next `write()`
is its rows) and `write()` as a stream of pixels.

```console
make qemu-b651-paint SECS=60
```

`devices/b651/tests/fb_paint.c` asks for the geometry, draws a gradient, three discs and a border **on the
processor**, and blits the frame in. The host's display then shows that picture — the same three colours, pixel for
pixel:

```text
fb_paint: the panel is 1125x2436, 4500 bytes a row, 32 bits a pixel
fb_paint: blit set for 2740500 pixels; writing 10962000 bytes
fb_paint: write() took 10962000 bytes
fb_paint: the picture is on the panel
```

That is the whole chain working: **user memory -> /dev/hakios-fb -> the framebuffer -> virtio-gpu -> the screen.**
It still rasterises nothing — the drawing is a loop in user space. What it establishes is that the display path is a
real one a program can use, not only a console.

`HAKIOS_FB_BLIT`'s rect and its pixels cannot arrive in one ioctl: XNU caps an ioctl argument at `IOCPARM_MAX`
(8 KiB) and a panel frame is 10.9 MB. So the rect is issued first and the next `write()` fills it, which is also how
a caller with its own row pitch hands a frame over.

### Two bugs worth knowing before the next driver

| what | effect |
|---|---|
| `case HAKIOS_X:` with an uncast `_IOW`/`_IOWR` constant | every `HAKIOS_FB_*` ioctl fell through to `ENOTTY` (`415c099`: "the EPD and NFC drivers only matched by luck of constant folding; this one did not"). The fix present in the tree is the cast to `u_long`, which is what a cdevsw's `d_ioctl` is handed (`bsd/sys/conf.h:105`), applied **at both ends**: at each definition (`((u_long)_IOR('F', 1, struct hakios_fb_info))`, `hakios.fb.c:69-71`; `HAKIOS_EPD_UPDATE`, `hakios.epd.c:173`) and at each case label (`case (u_long)HAKIOS_EPD_UPDATE:`, `hakios.epd.c:637`; `case (u_long)HAKIOS_FB_INFO:`, `hakios.fb.c:272`). `HAKIOS_EPD_WAIT` is the one place still defined uncast (`hakios.epd.c:174`; the cast is at its label, `:639`), which is fine — and is why testing the label alone would have been enough. **On the mechanism: unverified.** The driver's own comment (`hakios.fb.c:66-68`) says a 32-bit *unsigned* case constant is promoted to a **signed** int, `0x80144602` → `0xffffffff80144602`. C converts each case label to the *promoted type of the controlling expression*, and that is `u_long` here, which would not sign-extend, so the stated mechanism looks wrong even though the symptom was real. Worth reproducing before it goes into a driver comment. |
| a `printf` that quotes the ioctl number | the fastest way to find the above: it printed the same value as the case label, which is what proved the value was fine and the label was not |

### Three bugs, all in the virtio ring, all of the same family

| what | effect | fix |
|---|---|---|
| `used->idx` read without `volatile` | the load was hoisted out of the spin loop, so every command looked unanswered | the `*(volatile uint16_t *)&` cast the other virtio drivers use |
| a late completion taken as this command's reply | a working `SET_SCANOUT` read as refused | match the descriptor id in the used ring |
| the first three commands answered anyway | a red herring: their own `memcpy`/register writes forced a reload | same fix as the first |

The general lesson, worth having before the next virtio driver: **`0` from a virtio command means nothing until you
know whether the device refused or never answered.** `gpu_trace=1` prints which.

## Rules of engagement

`SCOPE.md` gate 1 is still open (`sys.oem_unlock_allowed=0`, bootloader locked), so the phone cannot take an
unsigned image today regardless. That is a good reason to work here, and it does not make the phone safe:

- **Nothing in `devices/b651/` is a phone-writing path.** No script in this port flashes anything.
  `tools/restore.sh` is the only one that even mentions a write, and it prints the device commands rather than
  running them: its header says `THIS SCRIPT DOES NOT TOUCH A PHONE`, and `--run` gates only the two host-side file
  commands (`tools/restore.sh:1-11`). Any script that ever needs to write says so in its first line and in
  `GATE1.md`.
- **The phone is read-only** until something boots in QEMU. `adb` reads are fine; `fastboot` and `mtkclient` are
  not, until there is a payload worth risking.
- **When the simulator boots end to end**, that is the moment the phone becomes the next test — not before.

## What is still needed to make the simulator more truthful

| gap | what would close it | cost |
|---|---|---|
| real panel geometry and `/eink` timings | read the dtbo, or root | BROM access |
| real waveform LUTs | read the `waveform` partition | BROM access |
| the dark-on-paper console made reproducible | the two `COLOR_*` lines are in `xnu-src` only; add them to `xnu/hakios.patch` so `xnu/apply.sh` reproduces them | minutes |
| MTK UFS instead of virtio-blk | write the driver against `HARDWARE.md` | weeks |
| USB device mode | port MTU3 into Mu-Silicium | weeks |
| hetero big.LITTLE | QEMU cannot model A76+A55 asymmetry on `virt` | not possible |
