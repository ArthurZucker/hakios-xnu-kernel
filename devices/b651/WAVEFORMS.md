# The b651 panel's waveforms and grey levels — known, cited, and invented

Ground truth for the e-ink model in `qemu/epd.py`. The simulator's numbers are a plausible shape, not a
measurement (`qemu/epd.py:37`, `SIM.md:153-156`); this file says which parts of that shape are real, which are
borrowed from a *different* controller family, and which are guesses that should not be presented as panel behaviour.

```text
  EVERY CLAIM BELOW IS ONE OF THESE FOUR
  ------------------------------------------------------------------------------------------
  KNOWN    verified in this repo, with file:line
  CITED    a public document or driver source, named
  ASSUMED  plausible and useful, but no measurement of this panel
  UNKNOWN  no basis at all; do not invent a number
```

Bottom line: **this panel is almost certainly 16-level (4-bit) greyscale for its grey waveform, 1-bit for its fast
waveforms, and its real frame counts, frame period, residue and temperature table are all unread.** The simulator's
four mode *names* correspond to real e-ink concepts; none of its four numbers do.

---

## 1. What the four modes actually are

### 1.1 The modes themselves

`gc16`, `gl16`, `du` and `a2` are not arbitrary labels. They come from the E Ink / i.MX `mxcfb` waveform vocabulary,
and the *concept* each one names is public. The exact waveform data for this panel is not.

| mode | what it drives | phases | cost | ghosting | evidence |
|---|---|---|---|---|---|
| **gc16** | every pixel to one of 16 grey levels; a full clear, all pixels addressed | unknown count; a flashing mode necessarily includes a clear excursion | the most expensive grey mode | intended to be zero — it is the un-ghosting waveform | CITED: `WAVEFORM_MODE_GC16 = 0x2`, "High fidelity (flashing)" (`libremarkable/mxc_epdc_fb_test.c:66`); `openjdk/jfx EPDSystem.java:226`. KNOWN: Bigme's Android layer calls mode `4` a full refresh (`EPDC.md:55`), and `4` is the same value Allwinner's EPD uses for `EINK_GC16_MODE` (CITED, RT-Thread `disp_waveform.h`). |
| **gl16** | grey levels, but without the clearing flash; corrects against the previous level instead of blanking | unknown | between gc16 and du | small, and accumulates until a gc16/init | CITED at name level: `eink_glx16_disable` is a **real DT property of this phone** (`PHONE.md:64,69-71`) — the vendor driver has a switch for GL16. KNOWN: the simulator's gloss for it (`SIM.md:76`). Semantics beyond "grey without flash" are ASSUMED. |
| **du** | direct update: each pixel driven from its current level straight to its target; does not blank | unknown | cheap | leaves a visible shadow of the previous content | CITED: `WAVEFORM_MODE_DU = 0x1`, "Grey->white/grey->black" (`mxc_epdc_fb_test.c:65`); `EPDSystem.java:200`. |
| **a2** | monochrome animation: two levels only, driven hard; the fastest mode | unknown; the shortest | cheapest | the worst; it is a 1-bit mode pretending the panel has no greys | CITED: `WAVEFORM_MODE_A2 = 0x4`, "Fast black/white animation" (`mxc_epdc_fb_test.c:68`); `EPDSystem.java:241`. |
| *(init)* | the panel's power-on clear, submitted once at attach | unknown | expensive | resets everything | CITED: `WAVEFORM_MODE_INIT = 0x0`, "Screen goes to white (clears)" (`mxc_epdc_fb_test.c:64`); NXP's driver submits `mode_init` with `UPDATE_MODE_FULL` at init (`mxc_epdc_fb.c:4255`); KNOWN: `EPDC.md:140-141` requires exactly this. |

**What a LUT actually is.** Not a list of frames. It is a table indexed by *(previous level, next level)* whose entry
is a per-phase drive code, run for N frames at a temperature-selected index (`EPDC.md:22-24,51`). A real LUT therefore
has `levels²` rows. On a 16-level panel that is 256 rows — which is why the open `epdiy` reference tables are
"two 256-entry nibble maps" (`EPDC.md:51`). This is the single most important structural fact the simulator does not
model: `qemu/epd.py` has one scalar residue per *mode*, not a table per *(old, new)* pair.

### 1.2 Phases and frames: what is genuinely unknown

| question | answer | basis |
|---|---|---|
| how many phases in a waveform | **unknown** | No public source gives the b651 LUT. The 4-phase model is the raw-EPD mental model (`EPDC.md:51`), not this panel. |
| how many frames per mode | **unknown** | Nothing in the repo measures it; `qemu/epd.py` chose 24/16/6/2. |
| does gc16 flash | **yes** (conceptually) | CITED: "High fidelity (flashing)", `mxc_epdc_fb_test.c:66`. |
| does du/a2 flash | **no**, by definition of a partial mode | CITED at mode-description level; no explicit statement. Treat as ASSUMED-strong. |
| what the flash looks like (white-first vs black-first) | **unknown** for gc16; INIT is white-first | INIT "Screen goes to white (clears)" (`mxc_epdc_fb_test.c:64`). The simulator's black-then-white story (`qemu/epd.py:130-131`) is ASSUMED. |

### 1.3 The numbering is a mess — and the repo's mapping is partly wrong

There are three different numbering systems in play, and the port currently conflates them.

| system | values | source |
|---|---|---|
| mxcfb canonical (NXP / Kobo / rM) | INIT 0, DU 1, **GC16 2**, GC4 3, **A2 4**, AUTO 257 | `mxc_epdc_fb_test.c:64-68`, `EPDSystem.java:188-241` |
| Bigme's Android integers | 4 (GC16/full), 132 (GU16), 1029 (handwrite), 178, 180 | `EPDC.md:55`, `EPDC.md:162` |
| Allwinner's own EPD enums | GC16 0x04, A2 0x10, DU_RECT 0x402 | RT-Thread `disp_waveform.h` |

```text
  what hakios.epd.c currently does (devices/b651/drivers/display/hakios.epd.c:330-346)
  ------------------------------------------------------------------------------------
   waveform_mode  ->  model      comment in the code
        0/4       ->  gc16       "the one Android uses for a full refresh"   CITED (EPDC.md:55)
        132       ->  gl16       "grey-level update with no flash"           ASSUMED
        180       ->  du         "direct update"                            ASSUMED
        1         ->  a2         "animation"                                ASSUMED -- and collides
                                                                           with mxcfb's DU = 1
        1029      ->  EINVAL     handwrite is not in the switch, so Bigme's
                                 own fastest mode is refused
```

Two defects follow. First, `1` means **DU** in the mxcfb vocabulary that `EPDC.md:119` says the ABI copies, but the
driver maps it to **a2**; whichever is true, the code cannot be right for both. Second, `qemu/epd.py` labels a2 with
a value of 2 frames while mxcfb's `A2` is 4 — harmless, because epd.py indexes by name, but it shows the numbers were
never meant to interoperate. `180`'s meaning is the least supported: `EPDC.md:55` records it as one of Bigme's
integers without saying which mode it is.

**Recommendation:** treat the mapping as an open question, not a fact, and do not encode it anywhere that outlives the
simulator. The phone's HAL will settle it (`EPDC.md:162`).

---

## 2. The grey level question

### 2.1 What the repo says

| claim | where | status |
|---|---|---|
| the panel supports a "full 16-grey refresh" | `EPDC.md:188-190` ("a full 16-grey refresh is 4x–30x that") | KNOWN that the repo asserts it; **no primary citation is given for the 16**, so it is effectively CITED-by-name (GC16) |
| the phone's `/eink` node has `eink_bit_num` and `eink_data_len` | `PHONE.md:57-64` | KNOWN property names; **values are root-only and unread** (`PHONE.md:53-54,136-143`) |
| the framebuffer is 32-bit `B8G8R8A8` | `hakios.fb.c:15-16,49`; `hakios.virtio_gpu.c:107`; `SIM.md:206` | KNOWN |
| XNU writes "white on black" for the console | `qemu/epd.py:229-231` | KNOWN of the console, not of the panel |

### 2.2 What the public record says

| fact | source |
|---|---|
| EPDC-class drivers present an **8-bit greyscale** framebuffer to the controller (`GRAYSCALE_8BIT`, `case 8:` sets all three channel offsets to 0) | CITED: `mxc_epdc_fb.c:4670-4690`, `mxcfb.h:82-85` |
| The ABI also defines a 4-bit greyscale format (`GRAYSCALE_4BIT`) — i.e. 16 levels is a first-class transport format | CITED: `mxcfb.h:84-85` |
| MediaTek's own EPD stack selects the **panel bit depth from the waveform file**: "4-bit stands for 16 grayscale, 5-bit stands for 32 grayscale" | CITED: RT-Thread `disp_waveform.c` `get_eink_panel_bit_num()` (Allwinner HAL, same property name `eink_bit_num` as this phone's DT) |
| GC16 = "16" grey levels; GC4 = 4; A2/DU are monochrome-ish | CITED: mode names + `mxc_epdc_fb_test.c:64-68` |

### 2.3 Plausible answers, with reasoning

| hypothesis | plausibility | why |
|---|---|---|
| **16 levels (4-bit)** | most likely | The full-refresh mode is named GC**16**; the repo already says "16-grey" (`EPDC.md:189`); `GRAYSCALE_4BIT` exists in the transport ABI; `eink_bit_num` exists as a property, implying a variable depth worth declaring. |
| 32 levels (5-bit) | possible, less likely | Allwinner's HAL supports it (`EINK_BIT_5`, used for the `ED060XD4` panel). No evidence this panel is 5-bit. |
| 8-bit internal, 4-bit output | plausible | An 8-bit greyscale buffer is what NXP's EPDC consumes; dithering flags (`EPDC_FLAG_USE_DITHERING_*`, `mxcfb.h`) let a controller quantise an 8-bit source down to a shallower panel. |
| 1-bit only | **false for gc16**, true for a2 | A2 is "fast black/white animation" (`mxc_epdc_fb_test.c:68`). |

**Answer to "does the repo say":** it says 16, but by mode name, not by reading `eink_bit_num`. The honest range is
**16 levels, with 32 a distant second and any answer requiring the `/eink` values.** The simulator should default to 16
and label it ASSUMED.

### 2.4 The quantisation chain the simulator omits

A real panel does not see colour, and does not see 8 bits per channel. It sees a level index into a LUT.

```text
  XNU / Android                    vendor EPD HAL            EPDC                 panel
  -----------------------------    ----------------------    -----------------    ----------------
  B8G8R8A8 pixel                   (unverified step:         LUT row =           16 physical
  B,G,R,X  ->  luminance            colour -> grey,           f(prev_level,       grey levels
  (only Y survives; alpha and       possibly dither)          next_level)         (ASSUMED)
   chroma are discarded)

    0x00RRGGBB  --luma-->  8-bit Y  --quantise-->  4-bit L  --index-->  waveform  --drive-->  ink

  the simulator today:  RGB -> PIL -> blend() -> RGB.  No luma, no quantisation, no index.
  a boot frame reports "colours=65" (SIM.md:122), i.e. 65 distinct RGB triples on a panel that has 16 tones.
```

Three separate losses, only the first of which is obvious:

1. **Colour is discarded.** Everything collapses to luminance. A red-on-green error message is a contrast problem on
   this panel, not a colour problem.
2. **8 bits become 4.** Only 16 tones exist. A smooth gradient bands into 16 steps unless the pipeline dithers.
3. **The mode changes the effective depth.** A2 and DU drive two levels; the same framebuffer quantises to 1 bit in
   those modes and 4 bits in gc16. The simulator applies the *same* image to every mode.

### 2.5 What the simulator actually does today

`qemu/epd.py` never quantises. It carries PIL `RGB` images end to end (`qemu/epd.py:101,139`), so the output keeps all
8 bits per channel and reports dozens of colours (`SIM.md:122`). The residue model is a scalar blend
(`qemu/epd.py:138-139`) using `ImageChops.darker`, which encodes an assumption that "more ink = darker" — true for the
white-on-black console, **wrong for a normal grey UI**, where residue is not monotonic in luminance.

---

## 3. The numbers table

`source` is a `file:line` in this repo, a cited public document, or the literal words `ASSUMED — no measurement`.

### 3.1 Frames per waveform

| mode | value | source |
|---|---|---|
| gc16 | unknown; model uses 24 | `ASSUMED — no measurement` (`qemu/epd.py:39`, `SIM.md:75`) |
| gl16 | unknown; model uses 16 | `ASSUMED — no measurement` (`qemu/epd.py:40`) |
| du | unknown; model uses 6 | `ASSUMED — no measurement` (`qemu/epd.py:41`) |
| a2 | unknown; model uses 2 | `ASSUMED — no measurement` (`qemu/epd.py:42`) |
| *derived duration* | a2 33 ms, gc16 400 ms in the simulator | KNOWN of the simulator: `hakios.epd.c:184-195`, `logs/boot-epd.log` (`update 101: ... a2 (33 ms)`, `update 102: ... gc16 (400 ms)`) |

### 3.2 Flash behaviour

| mode | value | source |
|---|---|---|
| gc16 | flashes | CITED: "High fidelity (flashing)", `libremarkable/mxc_epdc_fb_test.c:66` |
| gl16 | does not flash | CITED at name level: `eink_glx16_disable` (`PHONE.md:64`); semantics ASSUMED |
| du | does not flash | `ASSUMED — no measurement` (implied by "direct update" and by `mxc_epdc_fb_test.c:65`) |
| a2 | does not flash | `ASSUMED — no measurement` (`mxc_epdc_fb_test.c:68` says only "fast black/white animation") |
| *flash sequence* | unknown; model says black then white | `ASSUMED — no measurement` (`qemu/epd.py:130-131`); INIT is white-clear per `mxc_epdc_fb_test.c:64` |

### 3.3 Residual ghosting

| mode | value | source |
|---|---|---|
| gc16 | 0% | `ASSUMED — no measurement` (`qemu/epd.py:39`). Physically it is *near* zero, not provably zero. |
| gl16 | 6% | `ASSUMED — no measurement` (`qemu/epd.py:40`) |
| du | 22% | `ASSUMED — no measurement` (`qemu/epd.py:41`) |
| a2 | 45% | `ASSUMED — no measurement` (`qemu/epd.py:42`) |
| accumulation | `min(0.9, ghost * (1 + 0.5*(history-1)))` | `ASSUMED — no measurement` (`qemu/epd.py:138`); the 0.9 cap and 0.5 slope are invented |

There is **no public percentage** for any of these. Any number here is a tuning knob, not a fact.

### 3.4 Per-frame timing

| quantity | value | source |
|---|---|---|
| waveform frame period | unknown; model uses 16.7 ms (60 Hz) | `ASSUMED — no measurement` (`hakios.epd.c:184`; `qemu/epd.py` derives fps from `--fps`) |
| why 60 Hz might be right | MediaTek's EPD **DPI shim** on other SoCs reports a 60 Hz vsync | CITED as prior art, **not this panel**: `EPDC.md:50` (`extd_epd.c`). A DSI/DPI frame rate is not necessarily the waveform engine's frame period. |
| total update duration | 20–900 ms, waveform-dependent | `CITED` range stated in `EPDC.md:15,35`; the bounds are inherited from NXP-class hardware |
| fastest documented partial | ~30 ms (Bigme handwrite mode 1029) | `EPDC.md:188-189`, citing `inksdk architecture.md:79` |
| full 16-grey refresh | 4x–30x the handwrite mode, i.e. roughly 120–900 ms | `EPDC.md:189` — derived, not measured |
| real timing source on this phone | `eink_fresh_hz`, `eink_max_fps`, `eink_dynamic_fps` exist in `/eink` | KNOWN property names (`PHONE.md:62`); **values unread** |
| **per-frame timing** | **unknown** | no basis; the model's frame count x 16.7 ms is not a measurement |

### 3.5 Temperature dependence

| quantity | value | source |
|---|---|---|
| does temperature change the waveform | **yes** | CITED: `MXCFB_SET_TEMPERATURE`/`temp` field (`mxcfb.h:136-137,188`); `TEMP_USE_AMBIENT` (`mxcfb.h:101`); KNOWN: `EPDC.md:147-149` |
| how the driver uses it | table lookup by range, not a raw number | `EPDC.md:147-149`, citing `mxc_epdc_fb.c:1838-1867` |
| default temperature | 20 C | KNOWN of the port: `hakios.epd.c:115`; source `mxc_epdc_fb.c:65-66` — **not a reading of this panel** |
| number of temperature ranges for this panel | **unknown** | NXP's blob has a `trc` count (`EPDC.md:158-159`); this phone's `waveform` partition is unread |
| temperature buffer in memory | `mblock-25-eink_temp_buf` exists | KNOWN (`PHONE.md:77`) — a real, reserved buffer the vendor stack uses |
| effect in the simulator | **none** — temperature is stored and never used | KNOWN: `hakios.epd.c:459-473` sets `epd_temperature`; `epd_start_model()` (`:377-392`) never reads it. `qemu/epd.py` has no temperature parameter at all. |

### 3.6 Facts the model also gets for free and should not lose

| fact | value | source |
|---|---|---|
| panel geometry | 1125x2436 | KNOWN of the loader mode (`SIM.md:195-197`, `qemu/epd.py:31-34`); `/eink` values unread |
| framebuffer format | B8G8R8A8, 32 bpp | KNOWN (`hakios.fb.c:15-16,308`) |
| LUT indexing | (previous, next) -> per-phase code | CITED `EPDC.md:51` |
| panel state | last-driven level per pixel + temperature + partial count | KNOWN as the design statement: `EPDC.md:18-19` |
| LUT storage | `waveform` partition + `mblock-20-waveform` | KNOWN (`PHONE.md:41,76`) |
| VCOM | `vcom` partition, per unit, two copies | KNOWN (`PHONE.md:42,76-77`) |
| wrong LUT/VCOM can damage the panel | yes | KNOWN: `EPDC.md:177-178` |

---

## 4. What a faithful model would change in `qemu/epd.py`

Ordered by how much fidelity each buys per line of code. "Evidence to justify" is what would turn a guess into a
model.

| # | change | why it matters | evidence today | what would justify it |
|---|---|---|---|---|
| 1 | **Quantise the output to N levels** (luma, then 16 steps; 2 steps for a2/du) | The panel physically cannot show the 65+ colours the model emits (`SIM.md:122`). Banding and text antialiasing are the visible consequences. | KNOWN that the buffer is B8G8R8A8 (`hakios.fb.c:15`); CITED that EPDC eats 8-bit grey and panels are 4-bit (`mxcfb.h:82-85`, RT-Thread `get_eink_panel_bit_num`). **N = 16 is ASSUMED.** | Reading `eink_bit_num` from `/eink`, or the waveform file's panel type. Until then: 16, configurable, labelled. |
| 2 | **Make residue per-pixel, not per-mode** | A scalar blended over the whole rect cannot express "the pixels I never drove still hold the old image", which is the dominant real artefact. The repo already states the correct state vector: "last-driven level per pixel" (`EPDC.md:18`). | KNOWN as a design statement (`EPDC.md:18`); the current `ImageChops.darker` blend is a white-on-black-only approximation (`qemu/epd.py:138-139`). | Nothing further for the *architecture* — it is the stated model. The per-pixel coefficients still need a measurement. |
| 3 | **Quantise per mode**: a2/du to 1 bit, gc16/gl16 to N | A2 is a monochrome mode (`mxc_epdc_fb_test.c:68`); applying a 4-bit image to it overstates its quality. | CITED mode descriptions; the simulator uses one image for all modes (`qemu/epd.py:125`). | The 1-bit claim is already well supported. The a2 *dithering* choice is a separate guess. |
| 4 | **Model the flash as transitions, not an instant blank** | `settle()` returns the clean `new` image immediately when `flashes` is true (`qemu/epd.py:132-134`), so the model never renders the blink. The docstring even promises intermediate flash frames from `epd_view.py` (`qemu/epd.py:130-132`) — `epd_view.py` has none (no `flash`/`blink` in it). A user watching the simulator therefore never sees the most conspicuous e-ink behaviour. | CITED that gc16 flashes (`mxc_epdc_fb_test.c:66`); the black-then-white direction is ASSUMED. | The existence and duration of the flash; direction and phase count need the LUT. |
| 5 | **Temperature as a parameter, with no invented curve** | The driver stores a temperature and never uses it (`hakios.epd.c:459-473`), so "temperature compensation" is currently a claim with no code behind it. | KNOWN that temperature matters (`EPDC.md:147-149`). | The blob's `trc` table. Until then, accept `--temp` and apply a *linear, labelled* placeholder — never present it as measured. |
| 6 | **Replace frame counts with durations** | A LUT has a length in frames; the model has an invented count times an invented 16.7 ms. The product happens to land in the cited 20–900 ms range (`EPDC.md:15`), which makes it look validated when it is not. | CITED range only (`EPDC.md:15,188-189`). | A measured duration per mode, or `eink_fresh_hz`. |
| 7 | **Per-mode bit depth in the kernel model too** | `epd_model[]` carries a `residue` field (`hakios.epd.c:185-195`) that **no code ever reads** — `epd_start_model()` uses only `frames` and `flashes` (`:377-392`). Dead fields invite the reader to believe residue is modelled in the guest when it is not. | KNOWN (code inspection). | Delete it, or wire it to a per-pixel surface in the guest. Do not leave it as decoration. |
| 8 | **Do not treat the auto-policy thresholds as physics** | `FAST_AREA = 0.30`, `PAINT_LIMIT = 12`, the 5% a2 cutoff and the damage threshold of 8 (`qemu/epd.py:152,157-158,169`) are policy guesses. The repo itself says there is "no clean rule for when to clear" and that real drivers expose a counter rather than decide (`EPDC.md:181`). | ASSUMED — no measurement. | Keep them, but name them policy; a faithful model should expose the partial-update counter and let the caller choose, as `MXCFB_SET_AUTO_UPDATE_MODE` does (`mxcfb.h:189`). |

What **not** to change: the rect semantics (outside the rect nothing is driven) and the "no damage -> no update"
rule. Both are correct and are the reason the model is useful (`SIM.md:103`).

---

## 5. How to measure the real panel without touching it

The phone is read-only until something boots in QEMU (`SIM.md:310-318`), and the interesting artefacts are all behind
root anyway (`PHONE.md:120-124`). Three tiers, cheapest first.

### 5.1 What holds the answers

| artefact | where | what it yields | access |
|---|---|---|---|
| waveform LUTs | **`waveform` partition** (`PHONE.md:41`), also `mblock-20-waveform` (`PHONE.md:76`) | frames per mode, phases, temperature range table, possibly the panel's bit depth | BROM only |
| VCOM calibration | **`vcom` partition** (`PHONE.md:42`), `mblock-23-vcom` + `mblock-40-vcom` (`PHONE.md:76-77`) | per-unit VCOM; two copies implies primary + backup | BROM only; **never write** |
| panel timing + bit depth | `/eink` DT node: `eink_bit_num`, `eink_data_len`, `eink_fresh_hz`, `eink_max_fps`, `eink_dynamic_fps`, `eink_glx16_disable`, the `f..`/`l..`/`s..` triplets (`PHONE.md:57-64`) | **bit depth, real frame rate, panel type** — the three numbers this document is missing | root or dtbo; names are readable, values are not (`PHONE.md:53-54`) |
| controller identity + base | the phone's `dtbo` (`HARDWARE.md:106`, `EPDC.md:157`) | which EPDC, therefore which LUT format | root or dtbo |
| temperature buffer | `mblock-25-eink_temp_buf` (`PHONE.md:77`) | confirms the vendor stack reads a temperature into a reserved buffer | BROM |

### 5.2 What a read would require

1. **BROM access** — `mtkclient` with `mt6877_payload.bin` (`SCOPE.md`, gate 1; `GATE1.md:6-8`). macOS needs
   `libusb` + `sudo` and can hang on Apple Silicon; a Linux VM with USB passthrough is calmer (`GATE1.md:7-8`).
2. **The backup sequence first**, because that is the step that touches the phone (`GATE1.md:93-107`). Dump
   `boot_a`, `lk_a`, `preloader_a`, `seccfg`, `nvdata`, `nvram`.
3. **Then read, do not write**, `waveform` and `vcom`:
   ```console
   mtk r waveform waveform.img
   mtk r vcom     vcom.img
   ```
   `GATE1.md` does not list these two; they would be additions to its step 5 and carry no write risk if used as `r`.
4. **Also pull the dtbo** (`GATE1.md:34-45`) to recover every `/eink` value — this alone answers §2 and §3.4.

Nothing in this project should ever `mtk w` a waveform or a VCOM. A wrong LUT causes DC imbalance and a wrong VCOM
can permanently damage the panel (`EPDC.md:177-178`).

### 5.3 What can be inferred without the phone

| route | yields | confidence |
|---|---|---|
| **Download Bigme's OTA offline** (`marek-vybiral/bigme-ota-client`, named in `GATE1.md:112-113`) and unpack it on the host | Possibly the `waveform` image itself, if the OTA ships full partition images rather than deltas; if not, the EPD HAL `.so`, whose strings may carry the mode integers and a panel part number | Medium — untested, and explicitly a network download, not a phone operation |
| **`getprop` / `ls -l /dev/block/by-name` over `adb`** | partition *sizes* (a waveform dump size bounds the LUT count), build strings, any `ro.boot.*` panel hint | Low-medium — `PHONE.md:123` says the block nodes are unreadable to `shell`; `GATE1.md:60-68` implies the listing may still work. Worth one read. |
| **String-search the vendor kernel / EPD HAL from a downloaded firmware** | `eink_bit_num` usage, default temperature, mode integers | Medium, same offline route |
| **Public waveform files of the same class** (NXP `.fw` `trc` header, `EPDC.md:158-160`; Allwinner waveform files with panel type + per-mode + per-temperature blocks) | the *shape* of the format and typical field values — not this panel's numbers | High as format prior, zero as measurement |
| **Panel part number** (from dtbo or the EPD HAL) then the vendor datasheet | grey levels, update times, temperature range | High if a datasheet is public; unknown whether it is |
| **The OS's own behaviour** (watch an unmodified phone refresh; time a page turn with a stopwatch) | durations per mode within ~50 ms | Low precision, but it needs no access at all and would at least falsify the model's 33–400 ms band |

### 5.4 What stays unknowable without the LUT

Even with `/eink` read, the per-*(old,new)* drive codes, the phase count, and the residue coefficients stay unknown:
they live only in the waveform blob. Any claim about them in the simulator should remain labelled ASSUMED.

---

## 6. What one read would answer most

```text
  read the dtbo / /eink          -> eink_bit_num (grey levels), eink_fresh_hz (timing),
                                    eink_glx16_disable (whether GL16 is even enabled on this unit)
  read waveform partition        -> frames, phases, temperature ranges, LUT structure
  read vcom partition            -> per-unit VCOM only; no waveform information
  touch nothing                  -> policy thresholds, model coefficients: all stay ASSUMED
```

The single highest-value item is **`eink_bit_num`**, because it converts the largest ASSUMED block in this document
(§2) into KNOWN, and it is one property value in a file the project already knows how to pull.

---

### Sources

Repo: `devices/b651/EPDC.md`, `HARDWARE.md`, `PHONE.md`, `SIM.md`, `SCOPE.md`, `GATE1.md`, `device.conf`;
`qemu/epd.py`, `qemu/epd_view.py`; `devices/b651/drivers/display/hakios.epd.c`, `hakios.fb.c`;
`drivers/gpu/hakios.virtio_gpu.c`; `logs/boot-epd.log`.

Public: `nxp-imx/linux-imx` @ `imx_4.9.11_1.0.0_ga` — `include/uapi/linux/mxcfb.h` (greyscale formats 82-85,
`mxcfb_update_data` 131-137, `mxcfb_waveform_modes` 152-159, ioctls 187-197),
`drivers/video/fbdev/mxc/mxc_epdc_fb.c` (init waveform 4255, 8-bit greyscale setup ~4670-4690, defaults 4859-4867);
`canselcik/libremarkable` `reference-material/mxc_epdc_fb_test.c:64-68`;
`openjdk/jfx` `EPDSystem.java:188-241`; RT-Thread `rt-thread`
`bsp/allwinner/libraries/sunxi-hal/hal/source/disp2/disp/de/lowlevel_v2x/disp_waveform.{c,h}`
(`get_eink_panel_bit_num`: 4-bit = 16 grey, 5-bit = 32 grey; `EINK_GC16_MODE 0x04`, `EINK_A2_MODE 0x10`).
