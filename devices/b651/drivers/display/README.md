# `/dev/hakios-epd`: the e-ink panel's controller (b651)

The character device the HiBreak Pro's panel is driven through, and the one thing this port needs that no
other port has had. The design is `EPDC.md` next to this directory, section 2; this directory is its first
step. **It is a skeleton: there is no working path to the panel in it yet.**

```text
  caller (a text console, later MiniIOMobileFramebuffer's drawRows)
        |  ioctl UPDATE (rect, waveform mode, update mode, marker, temperature)
        v
  /dev/hakios-epd  --->  [ waveform select ]  ---> [ collision check ]  ---> [ program the EPDC ]
   hakios.epd.c                 |                        |                          |
                                unknown                  unknown                    unknown
                          (the LUT blob)           (LUTs in flight)         (registers, IRQ)
```

## What is here

| File | Is |
|---|---|
| `conf` | the `CONFIG_HAKIOS_EPD` block: one `driver`, one file (`docs/drivers-config.md` has every word) |
| `hakios.epd.c` | `/dev/hakios-epd` under boot-arg `epd=1`: the four commands of `EPDC.md:117-122`, and the state they act on |

It is a character device and not an `IOFramebuffer` on purpose (`EPDC.md:68-92`): the kernel's display
object holds a frame that a scanout engine reads on a 60 Hz beat, and this panel holds its own image and
takes 20-900 ms to change one. What an e-ink panel has is a request and a completion, which is what these
four commands say.

| Command | Number | In | Out |
|---|---|---|---|
| `HAKIOS_EPD_UPDATE` | `_IOW('E', 1, struct hakios_epd_update)` | rect, update mode (partial/full), waveform mode, marker, temperature | — |
| `HAKIOS_EPD_WAIT` | `_IOWR('E', 2, struct hakios_epd_wait)` | marker (0: any) | the collision bitmap |
| `HAKIOS_EPD_SET_TEMP` | `_IOW('E', 3, int32_t)` | °C | — |
| `HAKIOS_EPD_POWERDOWN` | `_IOW('E', 4, int32_t)` | delay in ms, `-1` = never | — |

`write()` takes a stream of `struct hakios_epd_update`; `read()` is not implemented. Waveform upload,
auto-update mode and dithering are deliberately not here (`EPDC.md:124-127`).

## Nothing drives the panel, because the panel is not known

The head of `hakios.epd.c` lists this in full; the short version is that every value a driver needs is
still unread, and none of it is guessed:

```text
  what                          where it will come from                     state
  ----------------------------  ------------------------------------------  -------------------------
  the EPDC's address, window,   the phone's dtbo: /eink and /epd_pmic_det    not read (root-only
  IRQ, max update width         (PHONE.md:50-66)                            values; PHONE.md:136-143)
  the panel's width and height  /eink's eink_fb_width / eink_fb_height       not readable without root
  the waveform LUTs and their   the `waveform` partition, mblock-20-wf      format unverified
  temperature range table       (PHONE.md:41-42,73-77)
  power sequence, VCOM, the     the `vcom` partition; SYV776A @ I2C 0x62     which chip is unverified
  EPD PMIC                      or tps65185 (PHONE.md:83-84)                (PHONE.md:83-84)
```

So `epd_attach()` stops at the first thing it does not have and says which one it was; `UPDATE` and `WAIT`
answer `ENOTSUP`. `SET_TEMP` and `POWERDOWN` keep their arguments and do nothing with them — there is
nothing yet to power. Nothing in this file maps a register, writes a PMIC or a GPIO, or touches the panel.
That is the point: a wrong write to the EPD PMIC or a guessed VCOM can damage an electrophoretic panel
permanently (`EPDC.md:178`), so the order of work is to read first and write last.

## What has been checked

- Compiled only: `clang -fsyntax-only` with the kernel's recorded flags (`BUILD/obj/.../.CFLAGS`), no
  warnings. Not linked into a kernel, not run in QEMU, not run on the phone.
- `python3 tools/drivers-check.py` and `python3 tools/hakios-device.py check b651` pass.

## The next step

`EPDC.md:200-214`, steps 1-4: pull the phone's dtbo and read `/eink` and `/epd_pmic_det` for the
controller's identity and base; unpack the `waveform` and `vcom` partitions; then fill in `epd_hw` in
`hakios.epd.c` and give `epd_attach()`, `epd_submit()` and the interrupt handler (`epd_complete()`, described
at `EPDC.md:107` and deliberately absent here) something real to do. The measurable goal after that is
`SCOPE.md` gate 3: **a text console refreshes a full screen in under a second without ghosting artifacts.**
