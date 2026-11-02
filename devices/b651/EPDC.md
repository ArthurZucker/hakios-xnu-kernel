# The EPDC driver for the Bigme HiBreak Pro (b651): what a minimum one is, and what it cannot be

Scoping document for gate 3 of `SCOPE.md` (the e-ink display). Grounded in open prior art, cited by path and line.
Written before any access to the phone: every Bigme-specific statement below is either cited or marked **unverified**.

---

## 0. The shape of the problem, in one picture

```text
  what an ordinary phone display is          what this panel is
  --------------------------------           ------------------------------------------------
  framebuffer, holds its content             electrophoretic: pixels stay as last driven
  DRAM -> scanout engine -> DSI -> panel     DRAM -> EPDC -> panel, driven by a waveform LUT
  a frame is 16.7 ms of work                 a frame is 20 - 900 ms of work (waveform-dependent)
  write pixel = pixel changes                write pixel = schedule a voltage train per pixel
                                             (black->white, white->black, ... over N phases)
  no state beyond the buffer                 panel state: last-driven level per pixel + temperature
                                             + how many partial updates since the last clear
```

What an EPDC driver actually is: **a scheduler for voltage trains**, not a blitter. Everything it does is:
pick a waveform LUT row from (old level, new level), run it for the right number of frames at the right
temperature, and know when the panel needs a full clear because partial updates have accumulated ghosting.

```text
              update_request(rect, waveform, mode)
                          |
                 [ collision check ]  ---- overlap with an active LUT? delegate / merge
                          |
                 [ waveform select ]  ---- LUT number from (mode, temperature index)
                          |
                 [ program EPDC ]     ---- addr, stride, coords, size, LUT #, waveform mode
                          |
                 [ WAIT for LUT-complete IRQ ]   (20 - 900 ms, not 16.7)
                          |
                 [ panel idle ] ------> schedule POWERDOWN after pwrdown_delay
```

---

## 1. Prior art

| project | what it implements | licence | what is reusable here |
|---|---|---|---|
| `nxp-imx/linux-imx` `drivers/video/fbdev/mxc/mxc_epdc_fb.c` (5614 lines) | The reference EPDC driver: LUT banks, temperature range table from a firmware blob, partial vs full, collision detection, the POWERUP/POWERDOWN state machine, auto-update mode | GPL-2.0-or-later (file head) | **The whole software model.** Not the register writes (i.MX EPDC is NXP silicon). The ABI, state machine and edge cases are the spec for ours. |
| `.../mxc_epdc_v2_fb.c` (6998 lines) | Same for EPDC v2: 64 LUTs instead of 16, update width capped at 2047 px and split (`mxc_epdc_v2_fb.c:71`, `:3576`) | GPL-2.0-or-later | The width-splitting rule, needed on any panel wider than the controller's max burst region (the split loop itself reads clearest in `mxc_epdc_fb.c:3121-3134`) |
| `nxp-imx/linux-imx` `include/uapi/linux/mxcfb.h` | The userspace ABI: `struct mxcfb_update_data`, `MXCFB_SEND_UPDATE`, `MXCFB_WAIT_FOR_UPDATE_COMPLETE`, waveform-mode and flag constants | GPL-2.0 (**as published**; the ABI itself is not copied into XNU — see §4) | The *design*: one request struct carrying rect + waveform + update-mode + marker + temperature + flags. This is what every EPD userspace ever written expects. |
| `nxp-imx/linux-imx` `drivers/misc/mediatek/epd/{epd85hz/epd_drv.c, epd85hz/epd_debug.c, inc/epd_drv.h}` (as carried in `brandontict/daredevil-kernel`) | MediaTek's *own* in-tree EPD support, 2014-era: an I2C EPD power/VCOM chip (`FP_EPD` @ 0x54, `epd_drv.c:25-58`), DPI timing (`:242-273`), `power_on`/`power_off` stubs (`:281-328`) | GPL-2.0 (`extd_epd.c` head states it; `epd_drv.c` has no SPDX — treated as GPL-2.0 by provenance) | **Proof that MediaTek never put waveform logic in the kernel.** The driver only sets DPI timing and VCOM. The waveform engine is below it. |
| `Winkmoon/android_kernel_xinran_oppo_mt6877` `drivers/misc/mediatek/ext_disp/mt6833/extd_epd.c` (402 lines) | MediaTek's newer EPD path: a DPI "external display" that reports `DISP_IF_EPD` and a vsync of 60 Hz (`:227-252`), an `epd_ioctl` with three commands (`:277-300`), fence/vsync plumbing (`:71-150`) | GPL-2.0 (file head `:1`) | The SoC-side shape: EPD is a **DPI timing generator + fence fence**, and the LUT lives in a separate driver reached by `EPD_GetDriver()`. Also: **no mt6877 variant exists** (`HARDWARE.md:58`) — confirms the vendor EPD stack is out-of-tree. |
| `vroland/epdiy` `src/output_common/lut.c` | Working lookup tables for a raw EPD: the 4-phase drive model as concrete C tables (`:28-84` are two 256-entry nibble maps), plus the ESP32 timing that walks them | LGPL-3.0 | **The mental model of a LUT** made concrete: a LUT is a table indexed by (previous, next) giving a per-phase drive code. Read once; do not copy code (LGPL into a GPL-2.0 kernel is a licence question we do not need). |
| `waveshareteam/e-Paper` `EPD_7in5_V2.c` | The command sequence of a commodity EPD controller: reset, power-on, panel setting, **partial window via 0x91/0x90** (`:343-362`), refresh `0x12` (`:101`), power-off/deep-sleep `0x02`/`0x07` (`:462-464`) | No licence file in the repo (**unverified**; upstream Waveshare code is commonly MIT) | The *refresh state machine as a command list*, which is the closest thing to what a vendor EPDC register block looks like |
| `koreader/koreader` `frontend/device/kobo/*` | The real-world userspace side of `mxc_epdc_fb`: waveform selection per UI action, `MXCFB_WAIT_FOR_UPDATE_COMPLETE` discipline, "bypass wait" for panels where the ioctl is unreliable (`device.lua:778-802`), full-refresh-on-resume policy (`:183-184`) | AGPL-3.0 | Not code — **policy**. What a caller must do that a naive caller gets wrong. Informs §4's ioctl set. |
| `michael65536/hibreak-eink-manager` `core/EInkHelper.java`, `core/EInkManager.java` | The **Bigme vendor interface as it leaks to Android**: debugfs `/sys/kernel/debug/eink_debug/global_mode` (`EInkHelper.java:16,46`), `/sys/kernel/debug/eink_debug/vcom` (`:101,132`), an I2C EPD PMIC at `2-0062` named `SYV776A` (`:61,86,132`), and the refresh-mode integers (132 / 16 / 180 / 1 / 4 — `EInkManager.java:50-64,87-98`) | No licence file (**unverified**; treat as all-rights-reserved) | **The single most valuable file for reverse engineering.** It names the debugfs file, the mode numbers, and the I2C address of the panel power chip, without touching a phone. |
| `imedwei/inksdk` `docs/bigme-architecture.md`, `docs/bigme-sdk-reverse-engineered.md` | Reverse-engineered Bigme `xrz` stack: `/system/bin/handwrittenservice` root daemon (`architecture.md:37-45`), the EPD HAL mode integers (`architecture.md:79-81`: 1029 handwrite, 132 GU16, 4 GC16), `EINK_HANDWRITE_MODE = 1029` / `EINK_DEFAULT_MODE = 178` (`sdk-reverse-engineered.md:277-289`), the HWC layer-flag bits (`:291-320`), the per-app `DisplayPolicy.refreshMode` table (`:181-195`) | No licence file (**unverified**; docs only, do not vendor) | Confirms the architecture: **the waveform selection is a HAL-level integer**, and Bigme's daemon never talks to the EPDC directly. It talks to the HAL. |
| `komadorirobin/koreader-bigme-frontlight` | Bigme frontlight control from KOReader (cold/warm light via an Android settings provider) | MIT | Only confirms the device is a real, reachable KOReader target; **not** display-refresh prior art. Include to bound the "Bigme-specific" search honestly. |
| `loopback7084/hibreak_utils` | Firmware unpacking/repacking (`lpunpack` in `unpack.sh:13`) | GPL-3.0 | The tool route to `vendor_a.img`, where the EPD HAL and waveform blobs live (§3) |
| `joemk88/hibreak-android-auto-fix` `TECHNICAL.md` | Not a driver: how Bigme's SurfaceFlinger `textEnhance` shader corrupts virtual displays | No licence file (**unverified**) | Evidence of *where the e-ink post-processing lives*: **in SurfaceFlinger, not the kernel**. Also confirms the panel is driven through a normal Android graphics stack. |

**What is absent from all of the above, and is the novel part of this port:** nobody has written an EPDC driver
for XNU, and nobody has publicly written one for MT6877. `mxc_epdc_fb.c` is the only complete open EPDC
driver in existence; the MTK trees have a DPI shim and no waveform logic.

---

## 2. The minimum viable driver

### 2.1 Interface shape: **character device, not `IOFramebuffer`**

Recommended: `/dev/hakios-epd` with the house `cdevsw` pattern, plus **no changes to `MiniIOMobileFramebuffer.cpp`
until the panel can do one full refresh**. Reasons, in order:

| reason | evidence |
|---|---|
| HakiOS's own display object is already a software framebuffer that *holds* its content and is *redrawn* on a beat. E-ink cannot be either. Presenting e-ink as `IOFramebuffer` would put a panel that takes 200 ms per update behind a swap path built for 16.7 ms, with a 2-deep queue (`MiniIOMobileFramebuffer.cpp:196`, `SWAP_QUEUE 2`) and a 500/700 ms give-up (`:83-84`). Every swap would either time out or silently coalesce. | `MiniIOMobileFramebuffer.cpp:83-84,196,496-545` |
| The house has a working precedent for "kernel driver exposes a record format, userspace owns policy": `/dev/hakios-touch` is a `cdevsw` with `d_read`/`d_write` and no ioctl at all (`hakios.input.c:209-225`), with a shared header `xnu/files/bsd/sys/hakios.input.h:18-36`. The driver rules say a new driver starts with `STARTUP(EARLY_BOOT, ...)` and a boot-argument (`docs/porting/drivers.md:48,72`). | as cited |
| E-ink's real API is **not** "draw and present", it is "here is a rect and a waveform, tell me when it finished". That maps to `write()`/`ioctl()` + a completion notification, not to SwapEnd. `mxcfb_update_data` is exactly that request (`mxcfb.h:131-141`), and `MXCFB_WAIT_FOR_UPDATE_COMPLETE` is exactly that completion (`:191`). | `mxcfb.h:131-141,187-197` |
| `IOFramebuffer` is the right answer *later*, and only as a thin adapter: `MiniIOMobileFramebuffer`'s `drawRows()` already computes a dirty rectangle (`fDiffPixels`/`fDiffLeft..Bottom`, `MiniIOMobileFramebuffer.cpp:349-350,791-795`) and `PE_initialize_console(NULL, kPEGraphicsMode)` already hands the panel over from the text console (`:1259`). Once the character device works, `MiniIOMobileFramebufferShow()` (`hakios.gpu.cpp:67`, `hakios.adreno.c:2865`) becomes the place to *send a dirty rect to the EPD driver* instead of `memcpy` to the boot framebuffer. That is a ~50-line change, not a subsystem. | as cited |

So: **phase 1 character device, phase 2 `drawRows()` → dirty rect → EPD driver.** Do not write an
`IOMobileFramebuffer` subclass. The existing one is not a driver for a scanout engine; it is a memcpy shim
onto `vinfo.v_baseaddr` (`:529-532`), and `vinfo` comes from the loader's UEFI GOP (`xnuloader/loader.c:4342-4345`).

```text
                       now                              phase 1                    phase 2
  --------- ----------------------------  ----------------------------  ----------------------------
  producer  UEFI GOP linear fb            /dev/hakios-epd writer        MiniIOMobileFramebuffer
            (loader.c:4342-4345)                                        .drawRows() dirty rect
  kernel    MiniIOMobileFramebuffer       hakios.epd.c (cdevsw)         hakios.epd.c + 50-line
            memcpy to v_baseaddr                                        hook in drawRows()
  panel     whatever UEFI left             EPDC, one LUT at a time       EPDC, coalesced rects
```

### 2.2 Functions and entry points — and nothing more

Kernel side (`bsd/dev/hakios.epd.c`), boot-arg `epd=1`:

```c
/* life */
void hakios_epd_init(void);                       /* STARTUP; reads epd=, finds the node, maps regs */
static int epd_attach(void);                      /* panel reset, power rails, read EPDC version */
static void epd_powerup(void);                    /* the POWER_STATE_OFF -> ON half */
static void epd_powerdown(void);                  /* the ON -> OFF half (mxcfb calls this "POWERDOWN") */

/* the one thing that matters */
static int  epd_submit(const struct hakios_epd_update *u);  /* rect + waveform + mode -> LUT */
static void epd_complete(void *);                 /* LUT-done IRQ -> wake waiters, maybe powerdown */

/* cdevsw */
static int  epd_open/close(...);
static int  epd_write(...);                       /* stream of hakios_epd_update */
static int  epd_ioctl(...);                       /* the four below */
```

ioctls (names may change; the set must not grow):

| ioctl | in | out | why it cannot be omitted |
|---|---|---|---|
| `HAKIOS_EPD_UPDATE` | `struct hakios_epd_update` (rect, waveform mode, update mode partial/full, marker, temp) | — | The whole driver. Same fields as `mxcfb_update_data` (`mxcfb.h:131-141`) minus the alt-buffer/PxP fields, which exist for NXP's rotation engine and are not ours. |
| `HAKIOS_EPD_WAIT` | marker | collision bitmap | Callers that draw must know when the pixels are on the panel or they will overdraw a LUT in flight. `mxc_epdc_fb.c:3139-3197` is the reference. |
| `HAKIOS_EPD_SET_TEMP` | °C | — | Waveforms are temperature-dependent; without this the panel runs one table at all temperatures (`mxc_epdc_fb.c:1838-1867`). |
| `HAKIOS_EPD_POWERDOWN` | delay ms (`-1` = never) | — | The panel must not sit powered between updates; this is the delay knob of `mxc_epdc_fb.c:3199-3218` and the reason the state machine is two-state, not one. |

Deliberately **absent** in v1: waveform-mode *upload* (`MXCFB_SET_WAVEFORM_MODES`, `mxcfb.h:187`), auto-update
mode (`:189`), update scheme (`:194`), dithering, alt-buffer, `GET_WORK_BUFFER`. Each is a caller-side feature
layered on the four above; adding them before a full refresh works is the classic way this gate never closes.
Waveform LUTs come from a blob in v1 (see §3), so there is nothing to select yet.

### 2.3 The state machine, copied from `mxc_epdc_fb.c` and nothing else

```text
        OFF ---(update or pwrdown cancelled)---> ON ---(pwrdown_delay expired, no LUT active)---> OFF
         ^                                       |
         |                                       +-- LUTs active: 1..num_luts at once, each a rect
         +-- init: powerup, full refresh, powerdown  (mxc_epdc_fb.c:1273-1284)
```

Rules that are load-bearing and are easy to get wrong:

1. **A full refresh is the init waveform, not "partial on the whole screen."** `mxc_epdc_fb.c:4246` submits
   `wvf_modes.mode_init` with `UPDATE_MODE_FULL` at init. This is the sequence that un-ghosts the panel.
2. **Partial updates collide.** Two LUTs covering overlapping rects cannot run at once; the hardware reports it
   (`epdc_get_colliding_luts`, `mxc_epdc_fb.c:949-955`) and the driver must either serialise or hand the
   caller a collision mask (`:3139-3197`, and the flag `EPDC_FLAG_TEST_COLLISION`, `mxcfb.h:107`).
3. **Never power down a panel with a LUT in flight.** Idle is defined as *no LUT active and no pending
   update* (`mxc_epdc_fb.c:3792-3813`), and the shutdown path waits up to 8 s for it (`:3433-3450`).
4. **Temperature is a table lookup, not a number.** The blob carries a `trc` count of range bounds
   (`mxc_epdc_fb.c:4305-4312`) and the driver picks index by range (`:1850-1856`); default 20 °C
   (`:65-66`). Sending the raw °C to the controller is a common and silent bug.

---

## 3. What must be reverse-engineered, and where it might live

| unknown | why it blocks | most likely home | how to get it |
|---|---|---|---|
| **The EPD controller's identity and base address** | Nothing can be written without knowing whether this is an NXP-style EPDC, a MediaTek `disp_epd` DPI shim, or a third-party controller behind I2C/SPI. `HARDWARE.md:57-58` found **zero** `epdc`/`eink` hits in any MT6877 tree, so it is out-of-tree. | The phone's own DTB/DTBO. `HARDWARE.md:106` names this the single most useful next fact. A Bigme kernel module could also own it (`*.ko` in `vendor_a.img`). | Pull `/proc/device-tree` or the boot image's `dtb`/`dtbo`; grep for `epd`, `eink`, `bigme`, `xrz`, and any compat string not in the OPPO tree. |
| **The waveform LUT blob(s)** | The LUT is the panel's physics: without it there is no correct black, no correct grey, no correct partial. It is *the* proprietary artefact. | Almost certainly a file, not kernel code: the `mxc_epdc_fb` model loads a firmware blob at runtime (`request_firmware_nowait(..., "imx/epdc/epdc.fw", ...)`, `mxc_epdc_fb.c:4292-4294`) whose header carries LUT count and temperature-range count (`:213-242`). Bigme's equivalent is likely on `vendor_a.img` under a Bigme/xrz path, or in a `dtbo` partition blob. | `lpunpack` the super image (`loopback7084/hibreak_utils` `unpack.sh:13`), then look in `vendor/` for anything matching the panel part number or `*.wf`/`*.lut`/`*.bin` with a plausible header. Compare sizes across firmware versions: a file that never changes is firmware; one that changes is a table. |
| **The temperature range table** | Same physical argument as the LUT; and it is compiler-inseparable from it in the NXP format (bounds are the first `trc+1` bytes of the blob, `mxc_epdc_fb.c:4305-4318`). | Same blob, if the format is NXP-like. Otherwise a vendor daemon table. | Read the first bytes of the candidate blob and test the NXP interpretation: `trc` plausible (single digit), bounds ascending over a realistic panel range. |
| **Panel timing / `vscan_holdoff`, `sdoed_*`, `gdclk_*`, DPI clock** | Wrong timing means no image at all, or an image with visible tearing. These live in the NXP driver as a per-panel table, not in the DT (`mxc_epdc_fb.c:312-360`). MediaTek's equivalent is DPI params in the DPI shim (`epd_drv.c:242-273`). | The phone DT if it is DPI-based; otherwise the vendor kernel image (string-search the zImage for the numbers, or disassemble the init). | Start from the DPI model: if the DT has an `epd`-ish node with `pclk`/`hback-porch`/`vback-porch`, this is a DPI shim and the timing is already written down. |
| **The panel power sequence: rails, VCOM, reset GPIO** | Driving the panel without VCOM is how e-ink panels get permanently damaged; VCOM is stored per-unit in flash. | A small EPD PMIC on I2C. **Bigme evidence:** `2-0062` / `SYV776A`, with `SYV776A_store_vcom` and `SYV776A_turnOnOff_power` attributes (`EInkHelper.java:61,86,132`). MediaTek's old driver has the same shape (`FP_EPD` @ 0x54, VCOM register at offset 0x02, `epd_drv.c:25-32`). | The DT's `i2c@.../epd@62` node, plus the vendor PMIC driver in `vendor_a.img`. **Do not guess VCOM.** |
| **What Bigme's daemon actually does** | Determines whether we have to reimplement anything above the HAL. | `/system/bin/handwrittenservice` (root daemon, `inksdk` `architecture.md:37-45`); the EPD HAL library under `vendor/lib64/hw/`. | `nm -D`/`strings` the HAL `.so`; look for the waveform-mode integers (4, 132, 1029, 178, 180 — `architecture.md:79-81`, `EInkManager.java:50-64`) and for the debugfs path `/sys/kernel/debug/eink_debug`. |

**The proprietary line, stated plainly:** it is **not** the daemon. Bigme's daemon is a pen-input + dirty-rect
forwarder to the EPD HAL (`architecture.md:37-45`), and it can be replaced by 200 lines of our own. It is
**not** the refresh-mode integers: those are in a public reverse-engineering writeup and in a public Java
app. The proprietary piece is **the waveform LUT set together with the temperature range table, and the
EPDC/panel timing they were tuned for**. Everything else is either Android plumbing or standard EPD technique.

---

## 4. Risks, ordered

| # | risk | why it is this high | mitigation / what would retire it |
|---|---|---|---|
| 1 | **The controller is undriven by anything public and undocumented.** Zero hits for `epdc`/`eink` anywhere in MT6877 trees (`HARDWARE.md:57-58`); no mt6877 `ext_disp` variant (`:58`). | If it is a bespoke Bigme block, we are writing a register-level driver from a blob and a disassembly. | The phone DT is the gate. If it names a known controller (a Solomon Systech / E Ink / NXP-compatible part), the risk drops by an order of magnitude. If it does not, this is a research project. |
| 2 | **The LUT blob is opaque and possibly encrypted or per-unit.** | Without it the panel cannot be driven correctly; with a wrong LUT it can be driven *damagingly* (DC imbalance). | Find the blob first, before writing any driver. If it is obfuscated, that is a stop-and-reassess point, not a hard part. |
| 3 | **No power/sequencing documentation; VCOM is per-unit.** | Wrong VCOM or rail order can permanently damage an electrophoretic panel. Panel damage is unrecoverable. | Read the PMIC node in the DT; read VCOM from the vendor's own storage before writing anything; never write a VCOM value that was not first read. |
| 4 | **The first thing that must work is also the hardest: the panel is the boot console.** | `SCOPE.md` gate 2 says the loader must show a logo, but the panel needs an EPDC that the UEFI does not have (`SCOPE.md`, gate 2 bullet "a framebuffer"). | Accept a UART-only bring-up (gate 1 already allows it), and treat "panel works" as strictly after "kernel runs". |
| 5 | **Darwin's userland assumes a video-rate display.** | `MiniIOMobileFramebuffer` is built around a 60 Hz beat (`fb_hz`, `:439`), a 2-deep swap queue (`:196`) and a 500 ms "show it anyway" (`:83`). None of that survives 200 ms updates. | Do not target WindowServer. The deliverable is a text console at e-ink rates (§5). |
| 6 | **Partial-update ghosting accumulates and there is no clean rule for when to clear.** | Every EPDC driver in the wild has a heuristic; `mxc_epdc_fb` exposes the counter to userspace rather than deciding (`MXCFB_SET_AUTO_UPDATE_MODE`, `mxcfb.h:189`; KOReader's full-refresh-on-resume policy, `device.lua:183-184`). | Start with: full refresh on any update larger than N% of the panel, or every M partials. Tune only against the real panel. |
| 7 | **No temperature source.** The driver needs °C; HakiOS's thermal driver is Qualcomm-only and off for b651 (`device.conf` `status thermal missing`). | Waveform at the wrong temperature is wrong contrast, not a crash — a slow, confusing failure. | v1: `HAKIOS_EPD_SET_TEMP` from userspace, default 20 °C (`mxc_epdc_fb.c:65-66`). v2: MTK LVTS, which is its own gate. |
| 8 | **Licence contamination.** `mxc_epdc_fb.c` is GPL-2.0-or-later; XNU here is APSL-2.0 with GPL-2.0 bits only where the repo already says so. | Copying the driver body is not permissible; copying the *design* is. `inksdk` has **no licence file** and must not be vendored. | Reimplement from the ABI and state machine, as this document does. Cite, do not copy. This is why §1 separates "reusable" from "read once". |

### What "done" cannot mean

A Darwin WindowServer at video rates on this panel is not a late deliverable; it is **not a deliverable**.
The panel's fastest documented waveform on this family is ~30 ms for a small partial region (Bigme's
`MODE_HANDWRITE = 1029`, `inksdk` `architecture.md:79`) and a full 16-grey refresh is 4×–30× that. A
compositor that assumes it can present a new full frame every 16.7 ms will either stall on the LUT or
tear; there is no third behaviour. `SCOPE.md` gate 3 already states this ("A text console or a bespoke
userland is"), and the honest success criterion is the one written there:

> a text console refreshes a full screen in under a second without ghosting artifacts

Everything in this document is sized to that, and to nothing larger.

---

## 5. What to do first, in order

```text
 1  pull the phone's DTB/DTBO and /proc/device-tree     -> identifies the controller (retires risk 1)
 2  lpunpack vendor_a.img, find the waveform blob        -> the LUT + temperature table (risk 2)
 3  read the EPD PMIC node; read VCOM, do not write it   -> power sequence (risk 3)
 4  write hakios.epd.c against a stub "controller" that
    logs the register writes                             -> compiles and runs in QEMU
 5  one full refresh on the phone (init waveform)        -> gate 3, first half
 6  partial refreshes + dirty-rect coalescing from
    MiniIOMobileFramebuffer.drawRows()                   -> gate 3, second half
 7  text console at e-ink rates                          -> SCOPE.md gate 3 "done"
```

Steps 1–3 need the phone; 4 does not and should start now.
