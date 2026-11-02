# Scope: HakiOS on the Bigme HiBreak Pro (B651, MediaTek MT6877)

What this port is, what blocks it, and in what order to attack it. Written before any code was run on the phone.

**Verified facts vs. assumptions.** Verified: the model is `B651`, the SoC is MT6877 / Dimensity 900, the product
string is `hibreak_eea`, the hardware string is `mt6877` (Bigme's OTA stack and a KOReader userpatch both check
these). The phone is a Magisk-rootable Android device with unsigned `boot_a` patches in the wild. Assumed and
labelled below: core counts, the GPU model, the storage generation.

## The one-paragraph version

The common kernel's ARM64 work is SoC-agnostic and carries over. Everything else in it — the PMIC arbiter, the
thermal sensors, cpufreq, socinfo, UFS, the audio DSP, the modem's IPA/QMI path — is written against Qualcomm's
blocks and does not exist for a MediaTek SoC. Two things make this port harder than `redwood`: the firmware that
starts the loader (Mu-Silicium has no MT6877 platform), and the display, which is e-ink and needs an EPD controller
driver that exists in neither HakiOS, Mu-Silicium, nor XNU.

## What carries over, what does not

```text
layer                          state for MT6877                          work
------------------------------ ----------------------------------------- -------------------------------------
XNU patch (xnu/hakios.patch)   SoC-agnostic ARM64                        reuse as-is
xnuloader                      builds an FDT, lays out memory            reuse as-is
tools/, configs/, Makefile     generic                                   reuse as-is
Driver framework               conf/map/BOOT-ARG plumbing is generic     reuse; new drivers slot in
UEFI                           Mu-Silicium: zero MT6877 platforms        WRITE a platform (gate 2)
Display                        e-ink EPD + waveforms                     WRITE (gate 3), nothing to copy
GPU / Metal                    Adreno only                               NO PATH (see below)
PMIC / keys / charger          Qualcomm SPMI arbiter                     WRITE on PMIF
Thermal                        Qualcomm TM0/TM1 + RPMh                   WRITE on MTK LVTS
cpufreq                        Qualcomm cpufreqhw mailbox                WRITE on MTK PPM
socinfo                        Qualcomm SMEM item 137                    WRITE on MTK boot tag
UFS                            Qualcomm ufs-hc + ICE                     WRITE on MTK ufs-hc (read-only first)
Audio                          Qualcomm ADSP loader                      WRITE or defer
Modem                          Qualcomm MDM + IPA + QMI                  WRITE or defer
Wi-Fi / BT                     Qualcomm WCN6750 via ath11k-in-XNU        WRITE or defer
```

## The three gates

**Gate 1 — can an unsigned image run on this phone at all?** Decides whether anything else matters.

`mtkclient` ships `mt6877_payload.bin`, so BootROM access exists. Magisk-based rooting of the HiBreak Pro is
documented and OTA-delta bypassing is a solved problem in `marek-vybiral/bigme-ota-client`, which implies an
unlockable, patchable `boot_a`. But HakiOS's normal path (`fastboot boot`, RAM only, nothing written) is *not*
dependable on MediaTek's LK — `fastboot boot` of a foreign image is frequently refused or hangs on this family.

Plan for the flash path instead: back up `boot_a` with mtkclient, `fastboot flash boot` a HakiOS image, and accept
that the phone is in a recoverable-not-safe state. That contradicts HakiOS's "writes nothing by default" promise, so
it belongs in this device's README, not in the common one.

*Done when:* a HakiOS loader banner appears on the panel or over UART, once.

**Gate 2 — a UEFI for MT6877.** The largest single blocker, and the one with no shortcut.

Mu-Silicium supports two MediaTek families (MT6789 "G99", MT6765/68 "P65/G80"), both from one contributor, neither
finishing USB device mode or internal storage. There is no MT6877 platform to copy. What a platform needs, from the
existing MTK ports and from `Winkmoon/android_kernel_xinran_oppo_mt6877` (a same-SoC Linux tree with vendor
modules materialised in-tree, the best public source of MT6877 addresses):

- memory map from the phone's own boot tag / DT (`lk` passes it; `preloader`'s DRAM table is authoritative)
- UART pins for bring-up (`boot-args`-style early log is the only way to see anything before display works)
- a framebuffer: on this phone that means the EPD controller, i.e. gate 3 arrives before the first visible boot
- UFS init, or accept RAM-only with an external payload

*Done when:* Mu-Silicium's `mt6877Pkg` builds, shows a logo, and runs HakiOS's `BOOTAA64.EFI`.

**Gate 3 — the e-ink display.** No prior art in this ecosystem.

An e-ink panel is not a framebuffer that holds its contents. It needs: an EPDC driver (waveform lookup tables, LUT
banks, temperature compensation), a full-refresh vs partial-refresh state machine, and — on Bigme — the vendor's
refresh timing budget, which is what their proprietary daemon does. Apple's Darwin userland additionally expects a
display that can be drawn to at a video rate; e-ink cannot. Even with a working EPDC, Darwin's WindowServer is not a
realistic target on this panel. A text console or a bespoke userland is.

*Done when:* a text console refreshes a full screen in under a second without ghosting artifacts.

## The Metal question, stated plainly

HakiOS's Darwin-userland payoff comes from Mesa's freedreno driving Adreno through `hakios.adreno.c` and
`tools/mesa/hakios-drm/`. Mali has no equivalent here: panfrost is a Linux kernel driver and a different Mesa
backend. So either

- **layer 1 only** (HakiOS's own open kernel + your own tiny userland), where the GPU does not matter, or
- **write a Mali kernel driver + port panfrost to XNU's DRM shim**, which is a research project on its own, or
- **not this phone**.

There is no third option that ends in Darwin's UI on a Mali-G68.

## Order of work

| # | Step | Needs phone? | Blocking? |
|---|---|---|---|
| 1 | `adb pull /sys/firmware/fdt`, `mtkclient` partition dump, `getprop` capture | yes | the input for everything below |
| 2 | fill `soc.args` from that tree (`tools/soc-args.py` — currently Qualcomm-shaped, expect it to emit nothing useful) | no | yes |
| 3 | bootloader unlock + `boot_a` backup + flash-boot of a trivial image (gate 1) | yes | yes |
| 4 | Mu-Silicium `mt6877Pkg` skeleton: memory map, UART, payload handoff (gate 2) | partly | yes |
| 5 | EPDC driver: init, one full refresh, then text console (gate 3) | partly | yes |
| 6 | UFS read-only on MTK ufs-hc, then the loader's storage service | no | no |
| 7 | touch, keys via PMIF, battery | no | no |
| 8 | thermal, cpufreq, socinfo on MTK blocks | no | no |
| 9 | audio, Wi-Fi, modem — decide after 1–8 whether they are worth it | no | no |

Steps 1–3 are cheap and decide the project. Steps 4–5 are the ones measured in months.

## What is in this directory

```text
devices/b651/
  device.conf                 what the phone is; every status line is `missing` until something is proven
  SCOPE.md                    this file
  configs/b651_defconfig      common + mt6877 + b651
  configs/b651.config         every common phone driver left out (their addresses are the POCO's)
  soc/soc.conf                the MT6877 family's facts
  soc/configs/mt6877.config   cores/bootcpu only; the Qualcomm-shaped drivers stay off
```

`soc.args` is absent on purpose: it can only be filled from the phone's own device tree, and a fabricated one is
worse than none.

## Sources worth having open

| What | Where |
|---|---|
| MT6877 Linux kernel with vendor modules | `Winkmoon/android_kernel_xinran_oppo_mt6877` |
| BROM/DA access for this SoC | `bkerler/mtkclient` (`mt6877_payload.bin`) |
| Bigme's OTA / model strings / unlock notes | `marek-vybiral/bigme-ota-client` |
| HiBreak Pro firmware handling scripts | `loopback7084/hibreak_utils` |
| Existing MTK UEFI ports to copy from | `Project-Silicium/Mu-Silicium` (MT6789, MT6765/68 only) |
| HakiOS's own porting rules | `docs/porting/index.md`, `docs/porting/drivers.md` |
