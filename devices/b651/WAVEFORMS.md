# The b651 panel's waveforms and grey levels — known, cited, and invented

Ground truth for the e-ink model in `qemu/epd.py`. The simulator's numbers are a plausible shape, not a
measurement (`qemu/epd.py:45-46`, `SIM.md:153-156`); this file says which parts of that shape are real, which are
borrowed from a *different* controller family, and which are guesses that should not be presented as panel behaviour.

**Line-number citations into other files may be stale.** Other agents are editing `qemu/epd.py`,
`qemu/b651_epdc.c` and `devices/b651/drivers/display/hakios.epd.c` in the same working tree while this file is
written, and `hakios.epd.c` moved by 129 lines during one edit of this document. Where a citation points into a file
that is still moving, it names the **symbol** (`epd_model[]`, `EPD_MODEL_FRAME_MS`, `epd_model_of()`) and the line
number is a convenience, not the anchor. The line numbers into `EPDC.md`, `PHONE.md` and `SIM.md` are stable.

```text
  EVERY CLAIM BELOW IS ONE OF THESE FIVE
  ------------------------------------------------------------------------------------------
  KNOWN    verified in this repo, with file:line
  CITED    a public document or driver source, named
  MEASURED a reading from THIS panel (scope, photometer, timed refresh). None exist yet.
  ASSUMED  plausible and useful, but no measurement of this panel
  UNKNOWN  no basis at all; do not invent a number

  SIM      verified by RUNNING qemu/epd.py on this checkout. A fact about the code, not
           about the panel -- but it is not a guess either, and it is the one column in
           this document that is reproducible without a phone.
```

**There is no MEASURED number in this document, and there cannot be one yet.** Every figure here is `KNOWN`,
`CITED`, `SIM`, `ASSUMED` or `UNKNOWN`, and a scope, a photometer and the `waveform` partition are what it would
take to produce a `MEASURED` one (§5). The `SIM` column exists so that "what the model does" stops being confused
with "what the panel does": it is reproducible, and it is reproducible off Python, not off ink.

Bottom line: **this panel is almost certainly 16-level (4-bit) greyscale for its grey waveform, 1-bit for its fast
waveforms, and its real frame counts, frame period, residue and temperature table are all unread.** The simulator's
four mode *names* correspond to real e-ink concepts; none of its four numbers do. Its residue numbers are worse than
a guess in one specific way, proved in §3.7: at the 2-level depth the `du` and `a2` rows render, they cannot express
a fraction at all, so `du 22% / a2 45%` is not what either waveform does to a pixel in the model.

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
| what the flash looks like (white-first vs black-first) | **unknown** for gc16; INIT is white-first | INIT "Screen goes to white (clears)" (`mxc_epdc_fb_test.c:64`). The simulator's black-then-white story (`qemu/epd.py:333-336`, and `qemu/b651_epdc.c`'s flash branch) is ASSUMED. |

### 1.3 The numbering is a mess — and the repo's mapping is partly wrong

There are three different numbering systems in play, and the port currently conflates them.

| system | values | source |
|---|---|---|
| mxcfb canonical (NXP / Kobo / rM) | INIT 0, DU 1, **GC16 2**, GC4 3, **A2 4**, AUTO 257 | `mxc_epdc_fb_test.c:64-68`, `EPDSystem.java:188-241` |
| Bigme's Android integers | 4 (GC16/full), 132 (GU16), 1029 (handwrite), 178, 180 | `EPDC.md:55`, `EPDC.md:162` |
| Allwinner's own EPD enums | GC16 0x04, A2 0x10, DU_RECT 0x402 | RT-Thread `disp_waveform.h` |

```text
  what hakios.epd.c currently does (epd_model_of(), cited by symbol -- other agents are editing
  that file and its line numbers move)
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
| XNU writes "dark ink on a paper-white page" for the console | `qemu/epd.py:432-439` (`describe()`) | KNOWN of the console, not of the panel. It was white-on-black until the console's `COLOR_BACKGROUND`/`COLOR_FOREGROUND` were inverted; `describe()` takes the modal colour as the background so either way round is handled. |

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

  the simulator today:  RGB -> luma -> level index -> ink range -> RGB.  Luma and quantisation are
  done (`qemu/epd.py:285-306`); the residue is still one scalar per mode, not a per-(old,new) table.
  A boot frame reports "colours=65" (`SIM.md:122`) from an OLDER epd.py that did not quantise; run it
  today and a settled frame has at most GREY_LEVELS colours (16 measured, §2.5).
```

Three separate losses, only the first of which is obvious:

1. **Colour is discarded.** Everything collapses to luminance. A red-on-green error message is a contrast problem on
   this panel, not a colour problem.
2. **8 bits become 4.** Only 16 tones exist. A smooth gradient bands into 16 steps unless the pipeline dithers.
3. **The mode changes the effective depth.** A2 and DU drive two levels; the same framebuffer quantises to 1 bit in
   those modes and 4 bits in gc16. The simulator applies the *same* image to every mode.

### 2.5 What the simulator actually does today

`SIM` — this section is what `qemu/epd.py` on this checkout was run and observed to do, not what it was written to
do. Two earlier statements in this file were wrong and are corrected here.

**It does quantise, and it does discard colour.** `panel()` (`qemu/epd.py:285`) takes luma (`convert("L")`), snaps it
to the waveform's level count through a lookup table (`qemu/epd.py:300-305`), and maps it into `INK_BLACK..PAPER_WHITE`
(0x22..0xDD). Measured: a 256-colour ramp settled through `gc16` comes out with **16 distinct colours**, and the same
ramp through `a2` with **2**. So §2.4's "the simulator today: RGB -> PIL -> blend() -> RGB. No luma, no quantisation,
no index" describes an older `epd.py` and is retracted; the four-row table at §2.4 is still the right description of
what the *panel* does, and of what the simulator now approximates.

**The residue is per-pixel, but its magnitude is a knob with no measurement behind it.** `settle()`
(`qemu/epd.py:308`) blends with `ImageChops.difference` scaled per pixel by how far that pixel moved
(`qemu/epd.py:344-346`), so "a pixel that barely moved ghosts less" is modelled. `ImageChops.darker` in the same
expression means residue still pulls toward *more ink*, never toward a symmetric difference — correct on a
dark-on-paper console (`INK_BLACK` is the pigment state, which the console's own inversion of
`COLOR_BACKGROUND`/`COLOR_FOREGROUND` has now made true), and wrong for a grey photo or a dithered gradient, where
a light pixel over a dark one ghosts in the other direction. That is a real limitation, but it is a *shape*
limitation, not the "never quantises" one that was claimed here before.

**The `frames` knob does not reach any pixel.** `settle()` unpacks `frames, flashes, ghost = MODES[mode]`
(`qemu/epd.py:329`) and then never uses `frames`. Setting `gc16.frames` to 1, 6, 24 or 60 renders byte-identical
output for all four modes. The frame count is a label (`epd.py`'s own test page prints it at `qemu/epd.py:476`;
`epd_view.py` prints it in its caption) and a timing input for the *guest*, where `EPD_MODEL_FRAME_MS` in
`hakios.epd.c` multiplies it and `epd_start_model` is the only reader. It is never a visual input. §3.1 is therefore
about what the guest sleeps for, not about what the viewer draws.

---

## 3. The numbers table

Every row carries a **status** column, and no row anywhere in this section is `MEASURED`. The vocabulary is the
one at the head of the file: `KNOWN` and `CITED` for things that are written down somewhere, `SIM` for things
this checkout's code was run and observed to do, `ASSUMED` for the model's own tuning numbers, `UNKNOWN` where
there is no basis at all.

`site` is a `file:line` in this repo or a named public document, so a reader can check the row rather than trust
it. Where a `SIM` row reports a rendered number it was produced by importing `qemu/epd.py` and calling
`settle()`/`panel()`, and the command is given so it can be repeated.

### 3.1 Frames per waveform — **status: ASSUMED, and inert for the viewer**

| mode | status | value | site |
|---|---|---|---|
| gc16 | ASSUMED | unknown; model uses 24 | `qemu/epd.py:48` |
| gl16 | ASSUMED | unknown; model uses 16 | `qemu/epd.py:49` |
| du | ASSUMED | unknown; model uses 6 | `qemu/epd.py:50` |
| a2 | ASSUMED | unknown; model uses 2 | `qemu/epd.py:51` |
| — | SIM | **`frames` changes no pixel** in `epd.py`: 1, 6, 24 and 60 frames render byte-identical output for all four modes | §2.5; `settle()` unpacks it at `qemu/epd.py:329` and never reads it |
| — | KNOWN (of the guest) | these counts become the kernel backend's sleep: `frames * EPD_MODEL_FRAME_MS` | `hakios.epd.c`, `epd_model[]` and `epd_start_model` (cited by symbol: other agents are editing this file and its line numbers move) |
| — | SIM | a2 33 ms, du 100 ms, gl16 267 ms, gc16 400 ms — arithmetic at 16.7 ms/frame, not observation; the multiplication is visible in the guest's own print | `logs/boot-epd3.log:142` (a2), `logs/qemu-b651-epd.log:148` (gc16); §3.8 bounds the error |
| — | SIM | the same counts at other frame rates: the 60 Hz choice is load-bearing — at 85 Hz (`EPDC.md:49`) they give a2 24 ms and gc16 282 ms; at 50 Hz, 40 ms and 480 ms | arithmetic from the counts above; nothing in the repo measures the period |

The duration row is arithmetic, not observation, and it is a `SIM` row because the multiplication is visible in the
guest's own print. It is **not** a claim about the phone: with the counts unknown and the frame period unknown,
the number is two guesses multiplied together. §3.8 bounds how far it could be wrong.

**The 33 ms / 400 ms figures are only reachable because the model picks 60 Hz.** At the 85 Hz the repo's own prior
art names (`epd85hz`, `EPDC.md:49`) the identical counts give a2 24 ms and gc16 282 ms; at 50 Hz, 40 ms and 480 ms.
Neither is a measurement either — but it shows the derived duration is not a fact about the panel even in the
simulator's own terms.

### 3.2 Flash behaviour

| mode | status | value | site |
|---|---|---|---|
| gc16 | CITED | flashes | "High fidelity (flashing)", `libremarkable/mxc_epdc_fb_test.c:66` |
| gl16 | CITED at name level / ASSUMED semantics | does not flash | `eink_glx16_disable` exists as a DT property (`PHONE.md:64`), so the vendor knows GL16; "no flash" is the concept, not a reading |
| du | ASSUMED | does not flash | implied by "direct update" and by `mxc_epdc_fb_test.c:65`; no explicit statement found |
| a2 | ASSUMED | does not flash | `mxc_epdc_fb_test.c:68` says only "fast black/white animation" |
| *flash sequence* | ASSUMED | unknown; model says black then white | `qemu/epd.py:331-334`; INIT is white-clear per `mxc_epdc_fb_test.c:64`, so white-first is at least as plausible |
| — | SIM | forcing `gc16.residue=0.80` with `flash=True` still renders clean at history 1, 5 and 12: the `flashes` branch returns `new` before residue is consulted | `qemu/epd.py:330-334` |

The last row matters because it means `gc16.residue=0.00` is **decorative**: the zero is unreachable, because the
flashing branch never reads `ghost`. The knob is honest — a flashing waveform cannot show residue in this model —
but the "0%" in §3.3 should be read as "the model does not model it", not as "measured to be zero".

### 3.3 Residual ghosting — **status: ASSUMED, and only one of the four is a fraction**

| mode | status | knob | what the model actually renders | site |
|---|---|---|---|---|
| gc16 | **not modelled** | 0.00 | unreachable: the flash branch returns clean at any history, even with the knob set to 0.80 | `qemu/epd.py:330-334` |
| gl16 | ASSUMED | 0.06 | a real fraction: 6% of the swing survives at history 1, growing with the streak (§3.7) | `qemu/epd.py:344` |
| du | ASSUMED, **not a fraction** | 0.22 | either 0% or 100% of a full swing: 2 levels cannot hold 22% (§3.7) | `qemu/epd.py:50,66` |
| a2 | ASSUMED, **not a fraction** | 0.45 | either 0% or 100% of a full swing: 2 levels cannot hold 45% (§3.7) | `qemu/epd.py:51,66` |
| accumulation | ASSUMED | `min(0.9, ghost * (1 + 0.5*(history-1)))` | the 0.9 cap and the 0.5 slope are invented; the history at which the cap binds is 30 / 8 / 3 for gl16 / du / a2 | `qemu/epd.py:344` |
| — | SIM | the cap binds at | gl16 history 30, du history 8, a2 history 3 | arithmetic on `qemu/epd.py:344` |

There is **no public percentage** for any of these. Any number here is a tuning knob, not a fact, and on the
two-level modes it is not even a usable knob (§3.7).

### 3.4 Per-frame timing

| quantity | status | value | site |
|---|---|---|---|
| waveform frame period | **UNKNOWN** | no reading of this panel. The model's 16.7 ms is a choice, not a measurement | `hakios.epd.c` (`EPD_MODEL_FRAME_MS`), `qemu/b651_epdc.c:132` |
| model frame period | SIM | 16.7 ms (60 Hz) in the kernel backend: `B651_EPDC_FRAME_NS = 16666667` and `EPD_MODEL_FRAME_MS = 1000 / 60` | `qemu/b651_epdc.c:132`; `hakios.epd.c` (`EPD_MODEL_FRAME_MS`) |
| — | SIM | **`panel.fps` is not the frame period.** It is the *viewer's repaint cadence* (default 2.0 here, 4.0 in `epd_live.py`); it multiplies no waveform count | `qemu/epd.py:96,481-491`; `qemu/epd_live.py:550` |
| why 60 Hz might be right | CITED as prior art, **not this panel** | MediaTek's EPD **DPI shim** on other SoCs reports a 60 Hz vsync | `EPDC.md:50` (`extd_epd.c:227-252`). A DSI/DPI frame rate is not necessarily the waveform engine's period |
| why 85 Hz is also possible | CITED as prior art, **not this panel** | MediaTek's own in-tree EPD support ships as `epd85hz` — a directory *named for 85 Hz* | `EPDC.md:49` |
| total update duration | CITED | 20–900 ms, waveform-dependent | `EPDC.md:15,35`; bounds inherited from NXP-class hardware, not measured here |
| fastest documented partial | CITED | ~30 ms (Bigme handwrite mode 1029) | `EPDC.md:188-189`, citing `inksdk architecture.md:79` |
| full 16-grey refresh | CITED (derived) | 4x–30x the handwrite mode, i.e. roughly 120–900 ms | `EPDC.md:189` — a multiply of a vendor doc, not a timing |
| real timing source on this phone | KNOWN (names only) | `eink_fresh_hz`, `eink_max_fps`, `eink_dynamic_fps` exist in `/eink` | `PHONE.md:62`; **values root-only and unread** (`PHONE.md:53-54`) |
| **per-frame timing** | **UNKNOWN** | — | no basis; the model's frame count x 16.7 ms is not a measurement |

The honest summary of this table: **the frame period, the frame counts, and therefore every duration in the
simulator are unknown, and the two prior-art frame rates in the repo disagree (60 vs 85 Hz).**

### 3.5 Temperature dependence

| quantity | status | value | site |
|---|---|---|---|
| does temperature change the waveform | CITED | **yes** | `MXCFB_SET_TEMPERATURE`/`temp` field (`mxcfb.h:136-137,188`); `TEMP_USE_AMBIENT` (`mxcfb.h:101`); and `EPDC.md:147-149` |
| how the driver uses it | CITED | table lookup by range, not a raw number | `EPDC.md:147-149`, citing `mxc_epdc_fb.c:1838-1867` |
| default temperature | CITED from a *different* controller / KNOWN of the port | 20 C | `hakios.epd.c`, `HAKIOS_EPD_TEMP_DEFAULT` (by symbol); source `mxc_epdc_fb.c:65-66` — **not a reading of this panel** |
| number of temperature ranges for this panel | **UNKNOWN** | — | NXP's blob carries a `trc` count of range bounds (`EPDC.md:147-149`) and the bounds are the first `trc+1` bytes (`EPDC.md:159`); this phone's `waveform` partition is unread |
| temperature buffer in memory | KNOWN | `mblock-25-eink_temp_buf` exists | `PHONE.md:77` — a real, reserved buffer the vendor stack uses |
| effect in the simulator | SIM | **none**: the guest stores it and nothing reads it; `epd.py` has no temperature knob at all | `hakios.epd.c` sets `epd_temperature` in `HAKIOS_EPD_SET_TEMP` and `epd_start_model` never reads it; no `temp` key in `qemu/epd.py`'s `KNOBS` (`qemu/epd.py:78-97`) |

### 3.6 Facts the model also gets for free and should not lose

| fact | status | value | site |
|---|---|---|---|
| panel geometry | KNOWN of the framebuffer XNU draws (not the panel itself) | 1125x2436 | `qemu/epd.py:50`; `/eink` values unread |
| the panel it is scaled onto | KNOWN (`wm size`: `Physical size: 824x1648`, density 300, a 6.14" diagonal) | 824x1648 | MiniIOMobileFramebuffer scales the framebuffer onto it |
| framebuffer format | KNOWN | B8G8R8A8, 32 bpp | `hakios.fb.c:15-16,308` |
| LUT indexing | CITED | (previous, next) -> per-phase code | `EPDC.md:51` |
| panel state | KNOWN as a design statement | last-driven level per pixel + temperature + partial count | `EPDC.md:18-19` |
| LUT storage | KNOWN | `waveform` partition + `mblock-20-waveform` | `PHONE.md:41,76` |
| VCOM | KNOWN | `vcom` partition, per unit, two copies | `PHONE.md:42,76-77` |
| wrong LUT/VCOM can damage the panel | KNOWN | yes | `EPDC.md:177-178` |
| ink and paper luminance | ASSUMED | `INK_BLACK = 0x22`, `PAPER_WHITE = 0xDD`; renders as levels 34 and 221 | `qemu/epd.py:60-61` |
| grey depth | ASSUMED | 16 levels, 2 for du/a2 | `qemu/epd.py:59,66` |

### 3.7 Why `du 22%` and `a2 45%` do not mean what they say

This is the one place in this document where a number can be shown to be **wrong as implemented**, without any
measurement of the panel, because it is an internal inconsistency of the model. It rests on two facts that are
both `SIM`:

1. `du` and `a2` are quantised to **two** levels (`MODE_LEVELS` at `qemu/epd.py:66`), and the panel's ink range is
   34..221, so the only two states a pixel can hold are 34 (ink) and 221 (paper). The midpoint is 127.5.
2. `settle()` computes a residue-blended level and then passes it through `panel()` (`qemu/epd.py:352`), which
   snaps it to one of those two states. So a fraction of a swing is rounded to a whole swing or nothing.

```text
  residue knob swept, a2/du, black->white, history 1, rendered level:
    knob  0.00 0.10 0.20 0.30 0.40 0.45 0.50 0.60 0.70
    value  221  221  221  221  221  221  221  34   34      <- 221=clean paper, 34=fully ghosted

  so the knob is a THRESHOLD, not a fraction: it flips at grown > 0.5, and between flips
  nothing changes. du=0.22 and a2=0.45 are indistinguishable at history 1 (both 221); they
  differ only in WHEN they flip -- a2 at history 2, du at history 4 -- and are identical again
  from history 4 onward (both 34).
```

Measured consequences of that, all `SIM`:

| history | gl16 (16 levels) | du (2 levels) | a2 (2 levels) |
|---|---|---|---|
| 1 | 209 | 221 | 221 |
| 2 | 209 | 221 | **34** |
| 3 | 196 | 221 | 34 |
| 4 | 196 | **34** | 34 |
| 8 | 171 | 34 | 34 |
| 12 | 146 | 34 | 34 |

(`settle()` on a black frame under a white one, panel levels, 16 levels for gc16/gl16 and 2 for du/a2. Reproduce
with `python3 -c "import sys; sys.path.insert(0,'qemu'); import epd, PIL.Image"` and a four-line loop, or read the
`--demo` output, which prints the history-12 column.)

Three things follow, and they are the honest answer to "what should the residues be":

* **The a2/du distinction the model claims is only a difference in *when* a pixel flips, not in *how much*.**
  du=0.22 and a2=0.45 render the same level at history 1 (both clean) and the same level from history 4 on
  (both fully ghosted); they differ only in the two partials in between. So the `du`-then-`a2` ordering is
  reproduced only as an ordering of crossings, and `epd.py`'s own self-check passes because it compares residue
  *knobs* (`_selftest_residue`, `qemu/epd.py:573-580`), not rendered pixels — it asserts that a mode with a larger
  knob is "no cleaner", which a threshold law satisfies trivially.
* **A defensible a2/du residue must be a history threshold, not a percentage.** The physical meaning of "a2
  ghosts worse than du" on a 2-level mode is "a2 needs fewer partials before a pixel has visibly flipped". The
  cleanest calibration is to keep the ordering as an ordering and choose the *crossing*: du should survive more
  partials than a2. At the knob's current growth law, crossing history = `2*(0.5/ghost) + 1` while that is
  below 20, i.e. du 0.20 -> history 5, du 0.30 -> history 3, a2 0.45 -> history 2. Those three numbers are
  ASSUMED; what is *not* assumed is that they are thresholds.
* **gl16 is the only waveform whose residue is a real fraction**, because it is quantised to 16 levels: the
  resolution floor is 1 step of 15, `1/15 = 0.067`. Its 0.06 therefore sits **below one quantisation step** and
  rounds to the same render as 0.04..0.10 at history 1. A residue finer than 6.7% cannot be represented on a
  16-level panel at all — that is a property of the panel class, not of this panel, and it is a floor, not a
  measurement.

### 3.8 What the number could be: error bars, stated as intervals not guesses

The brief asks for calibrated values with an error bar. The honest error bar here is **wide enough that the model's
specific numbers are not distinguishable from many others**, and that is worth writing down precisely.

| quantity | model value | admissible interval | why this is the whole interval |
|---|---|---|---|
| frame period | 16.7 ms | **11.8 – 20.0 ms** | the two frame rates the repo's own prior art names are 60 Hz (`EPDC.md:50`) and 85 Hz (`EPDC.md:49`); 20 ms is the ordinary 50 Hz lower bound of any DPI timing. Nothing narrower is supported. |
| gc16 frames | 24 | **8 – 53** | must land in the cited 120–900 ms full-refresh band (`EPDC.md:189`) at 16.7 ms/frame. 8 x 16.7 = 134 ms, 53 x 16.7 = 885 ms. |
| gl16 frames | 16 | **2 – 53** | no distinct published duration for GL16; the only bound is the 20–900 ms class range (`EPDC.md:15`), i.e. 2..53 frames. |
| du frames | 6 | **2 – 53** | same bound; nothing in `EPDC.md` separates du from gl16 in duration. |
| a2 frames | 2 | **2 – 7** | the only waveform the repo has a *number* for is the handwrite partial at ~30 ms (`EPDC.md:188-189`), 2 frames; a2 must also be the fastest *named* mode, so it is bounded above by du, and by 120 ms/16.7 = 7 if one insists a2 also fit "fast". |
| gc16 residue | 0.00 | **not modelled** | the flash branch never reads it (§3.2). Physically it is near-zero, not provably zero. |
| gl16 residue | 0.06 | **0.00 – 0.47** at the panel's resolution | one quantisation step is 0.067; the *rendered* value only changes at multiples of it (measured: 0.04–0.10 render the same at history 1). The upper end is "less than half the swing" — past 0.5 a partial is not a partial, it is a failure to write. |
| du residue | 0.22 | **a crossing in 2 – 10 partials** | not a percentage at all on 2 levels (§3.7). The interval is the histories at which a knob of 0.10..0.60 first renders ink, measured: 10 down to 1. |
| a2 residue | 0.45 | **a crossing in 1 – 5 partials**, and strictly fewer than du's | same law; the constraint that makes a2 "worse than du" is that it crosses *earlier*, which is the only physically meaningful ordering on a 1-bit mode. |
| frame period x frames | 33 – 401 ms | **24 – 1060 ms** | the product of the two intervals above (11.8 x 2 = 24 ms, 20.0 x 53 = 1060 ms); the cited class range 20–900 ms (`EPDC.md:15`) sits inside it, which is why the model "looks validated" when it is not. |

**What this table does not claim.** It does not claim the model's numbers are wrong — 24/16/6/2 are inside the
admissible intervals and so is the 60 Hz period. It claims the intervals are so wide that the simulator cannot be
distinguished from a broadly different panel by any observable it renders, and that the *shape* of the residue (a
fraction on gl16, a threshold on a2/du) is the part that is wrong rather than the magnitude. Narrowing these
intervals requires the three readings in §5.1 and none of them is a scope: `eink_fresh_hz` settles the frame
period outright, `eink_bit_num` settles the depth, and the `waveform` partition settles the counts and residues.

---

## 4. What a faithful model would change in the two simulators

Ordered by how much fidelity each buys per line of code. "Evidence to justify" is what would turn a guess into a
model. **Rows 1-3 and 7 have already landed in the code and are kept here struck through**, because this file is the
argument those changes were made against and a reader should be able to see which of it is history.

| # | change | why it matters | evidence today | what would justify it |
|---|---|---|---|---|
| 1 | ~~**Quantise the output to N levels**~~ **DONE** — `panel()` luma -> level index -> ink range (`qemu/epd.py:285-306`) | The panel physically cannot show the 65+ colours the model emits. Banding and text antialiasing are the visible consequences. | KNOWN that the buffer is B8G8R8A8 (`hakios.fb.c:15`); CITED that EPDC eats 8-bit grey and panels are 4-bit (`mxcfb.h:82-85`, RT-Thread `get_eink_panel_bit_num`). **N = 16 is ASSUMED.** | Reading `eink_bit_num` from `/eink`, or the waveform file's panel type. Until then: 16, configurable, labelled. |
| 2 | ~~**Make residue per-pixel, not per-mode**~~ **DONE** — `settle()` scales residue by how far each pixel moved (`qemu/epd.py:344-346`); the C device does the same in ink space (`qemu/b651_epdc.c:283-302`) | A scalar blended over the whole rect cannot express "the pixels I never drove still hold the old image", which is the dominant real artefact. | KNOWN as a design statement (`EPDC.md:18`). The remaining `ImageChops.darker` limitation (residue pulls toward more ink only) is §2.5. | Nothing further for the *architecture* — it is the stated model. The per-pixel coefficients still need a measurement. |
| 3 | ~~**Quantise per mode**: a2/du to 1 bit, gc16/gl16 to N~~ **DONE** — `MODE_LEVELS` (`qemu/epd.py:66`) | A2 is a monochrome mode (`mxc_epdc_fb_test.c:68`); applying a 4-bit image to it overstates its quality. | CITED mode descriptions. **Measured effect: a 256-colour ramp through a2 comes out with 2 colours.** | The 1-bit claim is already well supported. The a2 *dithering* choice is a separate guess. |
| 4 | **Model the flash as transitions, not an instant blank** — half done | `settle()` in `epd.py` still returns the clean image immediately when `flashes` is true, so the **Python viewer** never renders the blink (`epd_view.py` has no `flash`/`blink`, 0 hits). **The QEMU device does model it**: `b651_epdc.c:388-411` drives black for the first third of the frames, white for the second, then the target, so a mid-update capture shows the blink. | CITED that gc16 flashes (`mxc_epdc_fb_test.c:66`); the black-then-white direction is ASSUMED. | The existence and duration of the flash; direction and phase count need the LUT. The remaining work is to make `epd.py` agree with `b651_epdc.c`, not to model it again. |
| 5 | **Temperature as a parameter, with no invented curve** | The driver stores a temperature and never uses it, so "temperature compensation" is a claim with no code behind it. | KNOWN that temperature matters (`EPDC.md:147-149`). | The blob's `trc` table. Until then, accept `--temp` and apply a *linear, labelled* placeholder — never present it as measured. |
| 6 | **Replace frame counts with durations** | A LUT has a length in frames; the model has an invented count times an invented 16.7 ms, and the Python viewer does not even use the count. The product lands in the cited 20–900 ms range (`EPDC.md:15`), which makes it look validated when it is not. | CITED range only (`EPDC.md:15,188-189`). | A measured duration per mode, or `eink_fresh_hz`. |
| 7 | ~~**Delete the dead `residue` field in the kernel model**~~ **DONE** — the field is gone from `epd_model[]` (`hakios.epd.c`, by symbol), so the guest no longer advertises a residue it does not model | Dead fields invite the reader to believe residue is modelled in the guest when it is not. | KNOWN (code inspection: `grep -c residue devices/b651/drivers/display/hakios.epd.c` returns 0). | — |
| 8 | **Do not treat the auto-policy thresholds as physics** | `FAST_AREA = 0.30`, `PAINT_LIMIT = 12`, the 5% a2 cutoff and the damage threshold of 8 (`qemu/epd.py:359,364-365,376`) are policy guesses. The repo itself says there is "no clean rule for when to clear" and that real drivers expose a counter rather than decide (`EPDC.md:181`). | ASSUMED — no measurement. | Keep them, but name them policy; the QEMU device already exposes the counter and never decides for itself (`qemu/b651_epdc.c:45-48`), which is the right shape. |
| 9 | **Make the two simulators agree on residue on 2-level modes** | They disagree today, and the disagreement is a correctness bug, not a style one: `epd.py` snaps the settled value back through `panel()` so a2/du render only 34 or 221; `b651_epdc.c` computes the residue in **ink space** and never snaps, so a2 renders 136/95/52 — greys a 2-level waveform cannot make. | SIM: transcribed the C arithmetic and the Python arithmetic; tables in §3.7 and below. | Pick one. The Python behaviour (snap) is the one that matches the level count; the C device's `b651_epdc.c:195-199` comment argues the ink-space pull is "a linear reparametrisation" and therefore equivalent — **it is not equivalent once the result is quantised**, which is the whole point of a 1-bit mode. |

What **not** to change: the rect semantics (outside the rect nothing is driven) and the "no damage -> no update"
rule. Both are correct and are the reason the model is useful (`SIM.md:99`).

### The two simulators disagree, and here are the numbers

Both are transcribed from source rather than run: the C device is written but not yet compiled into QEMU
(`qemu/b651_epdc.c`, "IT IS NOT COMPILED BY ANYTHING YET"), so its column is a reading of the arithmetic, not an
observation. The Python column was run. The swing is black (ink 34) to white (ink 221).

```text
  mode  residue   history   qemu/epd.py (snaps to levels)   b651_epdc.c (ink space, no snap)
  ----  -------   -------   -----------------------------   ---------------------------------
  gl16      6%       1      209                              209
  gl16      6%      12      146                              148            <- agree
  du       22%       1      221 (snapped up from 179)        179
  du       22%       4       34 (snapped down from 118)      118            <- DISAGREE
  a2       45%       1      221 (snapped up from 136)        136
  a2       45%       2       34  (snapped down from 95)       95            <- DISAGREE

  on the 16-level modes the two agree within one level (a 1/15 quantisation step);
  on the 2-level modes they diverge by up to 87 ink values, and the C device emits
  greys (179, 118, 136, 95) that a 2-level waveform cannot produce.
```

The fix is on the C side: `b651_epdc_settle_pixel()` (`qemu/b651_epdc.c:297-303`) should push its result through
`b651_epdc_ink(b651_epdc_level_index(...), wave->levels)` before storing it, so the residue cannot land between
bands. That is a one-call change in a file this document does not own; it is recorded here because §3.7's argument
("the 2-level modes can only flip") is true of `epd.py` and false of the device, and a reader who checks one will
otherwise conclude the other is consistent.

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
| **Public waveform files of the same class** (NXP `.fw` `trc` header, `EPDC.md:147-149,159`; Allwinner waveform files with panel type + per-mode + per-temperature blocks) | the *shape* of the format and typical field values — not this panel's numbers | High as format prior, zero as measurement |
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

## 7. What the simulator assumes

This section exists so that no figure in this document can be read as a datasheet. Everything below is what
`qemu/epd.py` runs on, and **none of it comes from a datasheet, a measurement, or this panel's waveform blob.**

```text
  every number the panel model runs on, and where it is written down
  ============================================================================
  knob            value     used for                          status
  ----------------------------------------------------------------------------
  gc16.frames       24     guest sleep only; no pixels       ASSUMED (inert in epd.py, §3.1)
  gl16.frames       16     guest sleep only; no pixels       ASSUMED (inert in epd.py)
  du.frames          6     guest sleep only; no pixels       ASSUMED (inert in epd.py)
  a2.frames          2     guest sleep only; no pixels       ASSUMED (inert in epd.py)
  gc16.flash      true     clean render, partial counter=0   CITED that gc16 flashes; order ASSUMED
  gl16.flash     false     residue branch taken              ASSUMED
  du.flash       false     residue branch taken              ASSUMED
  a2.flash       false     residue branch taken              ASSUMED
  gc16.residue    0.00     unreachable (flash branch)        ASSUMED, and provably unused
  gl16.residue    0.06     per-pixel residue, a fraction     ASSUMED; below one 1/15 step
  du.residue      0.22     per-pixel residue, a THRESHOLD   ASSUMED; crossing at history 4
  a2.residue      0.45     per-pixel residue, a THRESHOLD   ASSUMED; crossing at history 2
  grey.levels       16     quantisation for gc16/gl16        ASSUMED
  grey.black      0x22     darkest ink (renders as 34)       ASSUMED
  grey.white      0xDD     lightest ink (renders as 221)     ASSUMED
  du.levels          2     quantisation for du               CITED (A2/DU are black/white modes)
  a2.levels          2     quantisation for a2               CITED
  panel.fps        2.0     how often the VIEWER repaints     a choice, not the panel's frame rate
  ============================================================================
  and outside the knob table:
  frame period    16.7 ms  only in the guest (EPD_MODEL_FRAME_MS) ASSUMED; maybe 85 Hz (EPDC.md:49)
  history growth  min(0.9, ghost * (1 + 0.5*(history-1)))        ASSUMED; both constants invented
  auto policy     FAST_AREA 0.30, PAINT_LIMIT 12, a2 < 5%        ASSUMED; policy, not physics
  damage threshold  luma delta > 8                                ASSUMED; policy
  ============================================================================
```

Two properties of this table are worth stating plainly, because they are the difference between a model and a
mock-up:

1. **`qemu/epd.py` can be recalibrated without editing code.** Every row above is a key in `KNOBS`
   (`qemu/epd.py:78-97`) and is overridden by `qemu/panel.json` if that file exists (`qemu/epd.py:220` loads it).
   `panel.json` on this checkout ships exactly the literals, so a `--calibrate` diff is a calibration change and is
   reviewable on its own. **No row in it is measured.**
2. **The guest's timing and the viewer's picture are driven by the same table but do not use the same fields.**
   The kernel backend multiplies `frames` by `EPD_MODEL_FRAME_MS` and ignores residue (it has no residue field at
   all); the Python viewer renders residue and ignores `frames` (`qemu/epd.py:329`). So "gc16 takes 400 ms and leaves 0%
   residue" is true of the system as a whole and true of neither component on its own — and both halves are
   ASSUMED.

What would make any of this `MEASURED` is in §5.1: `eink_fresh_hz` for the frame period, `eink_bit_num` for the
depth, and the `waveform` partition for the frames, phases and per-*level* residues. Nothing here requires a scope
on the panel's glass, which is the good news; nothing here can be settled by looking at the simulator, which is the
caution.

---

### Sources

Repo: `devices/b651/EPDC.md`, `HARDWARE.md`, `PHONE.md`, `SIM.md`, `SCOPE.md`, `GATE1.md`, `device.conf`;
`qemu/epd.py`, `qemu/epd_view.py`, `qemu/epd_live.py`, `qemu/panel.json`, `qemu/b651_epdc.{c,h}`;
`devices/b651/drivers/display/hakios.epd.c`, `hakios.fb.c`; `drivers/gpu/hakios.virtio_gpu.c`;
`logs/boot-epd3.log`, `logs/qemu-b651-epd.log`.

The `SIM` rows above were produced by importing `qemu/epd.py` from this checkout and calling `settle()` and
`panel()`; `python3 qemu/epd.py --demo` reproduces the history-12 column of §3.7 and is the cheapest way to check
that this document still describes the code.

Public: `nxp-imx/linux-imx` @ `imx_4.9.11_1.0.0_ga` — `include/uapi/linux/mxcfb.h` (greyscale formats 82-85,
`mxcfb_update_data` 131-137, `mxcfb_waveform_modes` 152-159, ioctls 187-197),
`drivers/video/fbdev/mxc/mxc_epdc_fb.c` (init waveform 4255, 8-bit greyscale setup ~4670-4690, defaults 4859-4867);
`canselcik/libremarkable` `reference-material/mxc_epdc_fb_test.c:64-68`;
`openjdk/jfx` `EPDSystem.java:188-241`; RT-Thread `rt-thread`
`bsp/allwinner/libraries/sunxi-hal/hal/source/disp2/disp/de/lowlevel_v2x/disp_waveform.{c,h}`
(`get_eink_panel_bit_num`: 4-bit = 16 grey, 5-bit = 32 grey; `EINK_GC16_MODE 0x04`, `EINK_A2_MODE 0x10`).
