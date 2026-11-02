# `mt6877Pkg` build order

Condensed from [`../UEFI-PLAN.md`](../UEFI-PLAN.md), which carries the path:line evidence
for every claim here. This file is the checklist; that file is the argument.

Sources: Mu-Silicium `6b3d90b79e19` ("Version 4.1", 2026-11-01) — its `Silicon/MediaTek/MT6789Pkg/`
(SoC, 13 files), `Platforms/Xiaomi/stonePkg/` (board, 10 files) — plus `../HARDWARE.md`
(MT6877 addresses) and `../PHONE.md` (what this unit reports).

**Nothing below has been attempted. No build has been run. No Mu-Silicium tree was
cloned for this scaffold** (see the README).

---

## 1. The milestone that gates everything

**UART only.** One line of text out of the phone, from `Sec.c`, on UART0 at `0x11002000`.

```text
milestone 1 = BootShim -> Sec -> PrintFirmwareVersion -> UART0 @0x11002000 -> adapter
```

No display, no storage, no operating system. Why UART: the panel is e-ink with no driver
in HakiOS, Mu-Silicium or XNU, and Mu-Silicium's `SimpleFbDxe` does not drive a display —
it registers a GOP over a `Display Reserved` buffer that something else already filled
(`UEFI-PLAN.md` §3.3). `../SCOPE.md:71-72` reaches the same conclusion independently.

Milestone-1 acceptance, in order, one experiment each:
1. `SILICIUM_UEFI.fd` builds (`python3 build_uefi.py -d b651 -r DEBUG`).
2. The three `Sec.c:47-48` lines appear on UART at 921600 or 115200 8N1.
3. The same on a flash-booted image (gate 1 passed first).
4. Only then: `Display Reserved` for real — needs gate 3 (e-ink).

---

## 2. Ordered build steps

One line each. `[C]` = copy an existing Mu-Silicium file and adjust; `[N]` = write new,
no template. Order is dependency order, not importance.

### 2a. SoC package, `Silicon/MediaTek/MT6877Pkg/`

| # | Step | Kind | Note |
|---|---|---|---|
| 1 | `MT6877Pkg.dsc.inc` | [C] MT6789 | Keep `USE_PHYSICAL_TIMER=1`, `-march=armv8.2-a+crypto+rcpc`, `PcdCoreCount 8`, `PcdClusterCount 2`, GIC `0x0C000000`/`0x0C040000`. Change ACPI OEM TableId/revision and SMBIOS part number. **Not in this scaffold** — the flat `mt6877.dsc` here stands in for it |
| 2 | `Library/PlatformSecLib/AArch64/PlatformHelper.S` | [C] unchanged | `PlatformAssemblyInitialize` is `ret` |
| 3 | `Library/PlatformSecLib/PlatformRegisters.h` | [C] unchanged | `WDT_MODE_KEY 0x22000000` is the MTK top-rgu key; MT6877 watchdog at `0x10007000` (`HARDWARE.md:62`) |
| 4 | `Library/PlatformSecLib/PlatformSec.c` | [C] unchanged | The only SEC-time hook: write the key to the region named `"WatchDog Timer"` |
| 5 | `Library/PlatformSecLib/PlatformSecLib.inf` | [C] + new `FILE_GUID` | MT6789 and MT6768 ship the *same* GUID; do not inherit blindly |
| 6 | `Library/MemoryMapLib/MemoryMapLib.c` | **[N]** | **In this scaffold.** The hardware contract: every driver finds its block *by name*. Milestone 1 needs the 6 regions in §3 |
| 7 | `Library/MemoryMapLib/MemoryMapLib.inf` | [C] | **In this scaffold** |
| 8 | `mt6877.dec` | [N] | **In this scaffold.** Declares only the PCDs `MemoryMapLib.c` uses |
| 9 | `Include/APRIORI.inc`, `Include/DXE.inc` | [C] stone, then trim | **In this scaffold**, trimmed to the milestone-1 component set |
| 10 | `mt6877.dsc` | [C] `stone.dsc` | **In this scaffold**, flattened: it is the platform DSC for now, not the SoC-class `.dsc.inc` |
| 11 | `Library/ClockImplLib/ClockImplLib.c` | **[N]** ~4000 lines | Milestone 1 uses `gClocks[] = {}`, `gClockCount = 0`. Real tables ported from `drivers/clk/mediatek/clk-mt6877*.c` in the vendor tree; **nothing transfers verbatim** from MT6789 |
| 12 | `Library/ClockImplLib/ClockImplLib.inf` | [C] + new GUID | generic |
| 13 | `Library/GpioImplLib/GpioImplLib.c` | [C] `MaxPin` only | MT6877 pin count must come from `pinctrl-mt6877.c`; 189 is the G99's, not a default |
| 14 | `Library/GpioImplLib/GpioImplLib.inf` | [C] + new GUID | generic |
| 15 | `Library/PmicWrapperImplLib/PmicWrapperImplLib.c` | [C] + adjust | Per-SoC register set: MT6789 and MT6768 already differ (`Wacs2Cmd 0x880` vs `0xC20`). pwrap is `0x10026000` (`HARDWARE.md:27`) |
| 16 | `Library/PmicWrapperImplLib/PmicWrapperImplLib.inf` | [C] + new GUID | generic |
| 17 | `Library/MsdcImplLib/MsdcImplLib.c` | [C] skeleton only | **MT6877 has no eMMC/SD in use** — UFS-only. Keep it compiled because `MediaTekPkg.dsc.inc:37` references it unconditionally; expect it to find no `MSDC-0` and skip |
| 18 | `Library/MsdcImplLib/MsdcImplLib.inf` | [C] + new GUID | generic |
| 19 | serial: bind `MdeModulePkg/Library/BaseSerialPortLib16550` + a 3-line MTK shim | **[N]** ~20 lines | MMIO, base `0x11002000`, stride 4, width 32; shim writes `MTK_UART_HIGHS=3` at `0x09*4` for baud ≥ 115200. No MTK board in Mu-Silicium binds a real serial port at all |

### 2b. Board package, `Platforms/Bigme/b651Pkg/` — *not scaffolded*

| # | Step | Kind | Note |
|---|---|---|---|
| 20 | `b651.dsc` | [C] `stone.dsc` | `PcdSystemMemoryBase 0x40000000`, stack base/size, SMBIOS strings, panel resolution (meaningless for e-ink, keep a placeholder), `MemoryMapLib` binding. As written, this scaffold's `mt6877.dsc` plays this role until the split is real |
| 21 | `b651.fdf` | [C] `stone.fdf` | Only the `!include` name changes; keep the DT block commented out (the loader builds its own DT) |
| 22 | `Library/KeypadDeviceLib/KeypadDeviceLib.c` | [C] 3 lines | Keys are on MT6359P **through pwrap, not PMIF** (`HARDWARE.md:31`), so `KEY_TYPE_PMIC` works only once TOPSTATUS is implemented |
| 23 | `Library/KeypadDeviceLib/KeypadDeviceLib.inf` | [C] unchanged | generic |
| 24 | `b651.dec` | [C] | 0-byte placeholder, as `stone.dec` is |
| 25 | `DeviceBuild.py` | [C] + adjust | `PackagesPath` must name `Silicon/MediaTek` |
| 26 | `Drivers/MT6359PmicDxe/` (`.c` + `.inf`) | **[N]** ~600 lines | MT6358PmicDxe is the template; the PMIC is a **different chip** and grepping Mu-Silicium for `MT6359` returns zero hits |
| 27 | `Resources/Configs/b651.toml` | [C] `stone.toml` | `build_uefi.py` refuses to build without it; `[uefi_fd] base/size` must match where LK places the FD |

**Counts:** 26 package files = 20 copied-or-adjusted (14 near-verbatim) + 5 written new for
milestone 1 (one of them ~4000 lines) + 2 out of scope (UFS, USB). Plus `b651.toml`.

---

## 3. Milestone 1: the smallest build

| Part | Milestone-1 contents | New code |
|---|---|---|
| `mt6877.dsc` / `.dec` / `.inc` | as scaffolded here | no |
| `PlatformSecLib` | 4 files copied | no |
| `MemoryMapLib.c` | **6 entries**: `UEFI FD`, `UEFI Stack`, `DXE Heap`, `WatchDog Timer`, `Pmic Wrapper`, `Display Reserved` (faked into DXE heap so `SimpleFbDxe`/`FrameBufferSerialPortLib` do not abort). Sizes by hand: FD 2 MiB, stack 256 KiB, heap 60 MiB | ~60 lines |
| `ClockImplLib` / `GpioImplLib` / `PmicWrapperImplLib` / `MsdcImplLib` | stubs (`gClocks[] = {}`, `gPlatformInfo` zeroed); their DXEs are **not** in the FV | ~10 lines each |
| serial | 16550 + MTK high-speed shim | ~20 lines |
| `APRIORI.inc` / `DXE.inc` | every storage/GOP/keypad component removed | trim only |

Deliberately **not** in milestone 1: `ClockDxe`, `GpioDxe`, `PmicWrapperDxe`,
`MT6359PmicDxe`, `MsdcDxe`, `EmmcDxe`, `SdDxe`, `Fat`, keypad. Each needs a table that
must be re-derived from the vendor kernel, or has nothing to bind to. Add them back one at
a time after step 2 of §1 passes, in this order:
`Gpio+Clock` → `PmicWrapper+MT6359Pmic` → storage.

---

## 4. The USB device-mode blocker, plainly

**The phone cannot be handed a system image over USB by this firmware, and that is not a
small gap.**

`../PHONE.md` shows the hardware can do it: the vendor kernel has `CONFIG_USB_MTU3=y`,
`CONFIG_USB_MTU3_DUAL_ROLE=y`, `CONFIG_USB_GADGET=y`, `CONFIG_USB_F_FS=y`. So this is a
software gap, not a silicon one.

Mu-Silicium has **zero** MediaTek device-mode code: 63 `usb` string hits across its
MediaTek trees, all in `ClockImplLib.c` (clock names) and `MT6358PmicDxe.c` (the
`ldo_vusb` regulator) — 0 INFs, 0 protocols, 0 PCDs. Both MediaTek devices in `Status.md`
are ❌ for USB host *and* device. Every ✅ in that file is Qualcomm, and those come from
prebuilt vendor binaries under `Binaries/`, written for Qualcomm's DWC3 and Qualcomm's
`UsbDeviceProtocol`.

HakiOS's loader binds `QCOM_USB_DEVICE_GUID` — literally Qualcomm's. On anything else
`usb_start()` prints `usb: device protocol not found` and the loader falls back to
one-shot mode.

| Path | Needs | Works on MT6877 today? |
|---|---|---|
| `xl ramdisk` (push the 2.5 GB system image after boot) | HakiOS's USB command loop over `QCOM_USB_DEVICE_GUID` | **No** |
| `fastboot boot` a HakiOS image | LK's own decision; `../SCOPE.md:49-50` records it as frequently refused or hanging on MTK LK | **Unknown / likely no** |
| `fastboot flash boot` then reboot | only LK's flash path; `../GATE1.md:93-131` | **The only evidenced path** — needs `seccfg` unlocked, proven by reading it |
| Boot from volume (`\EFI\hakios\payload.bin`) | UFS + FAT + a boot option | **No** — no MediaTek UFS driver, so BDS has no volume |

**Consequence: an `mt6877Pkg` today can only be delivered by writing an image to a boot
partition and rebooting. The payload must ride inside that boot image. It cannot be loaded
over USB and it cannot be read off the phone's own storage.**

Working around it means writing a MediaTek USB-device driver *and* either exporting it as
`QCOM_USB_DEVICE_GUID` or changing HakiOS's loader to speak a neutral protocol. Out of
scope here; `UEFI-PLAN.md` §3.1 has the full evidence chain.

---

## 5. Storage: UFS, and no MediaTek UFS driver exists

MT6877 storage is UFS 2.2 on `ufshci@11270000` (`HARDWARE.md:32-35`). Mu-Silicium's
`MsdcDxe` publishes only `gEfiSdMmcPassThruProtocolGuid` — an eMMC/SD driver — and there
is no `UfsDxe` under `Silicon/MediaTek` (only Samsung's, which needs a Samsung board).
So: no BlockIo → no `PartitionInfo` → no `FindPartition("logdump")` → no log; no
`SimpleFileSystem` → the volume-payload path is dead. The logo/no-boot BMPs come from the
FV, so the *visible* boot still works.

Plausible to port — the vendor driver binds `mediatek,mt8183-ufshci`, an open Linux
driver, DWC UFSHCI 3.x like `ufs-qcom` — but it is a new driver, not a copy. MPHY at
`0x11fa0000`, `refclk_ctrl = 2`.

---

## 6. Unknowns that would change this plan

| Unknown | Answers | From |
|---|---|---|
| HiBreak Pro's own device tree | real DRAM map, reserved regions, UART pinmux, e-ink base, PMIC rails | `adb pull /sys/firmware/fdt` (needs root/BROM on this unit) |
| Does LK accept `fastboot boot` | RAM-only vs flash-only | `../GATE1.md:70-91` |
| Does the bootloader leave UART0 muxed | whether milestone 1 is observable at all | phone + adapter |
| Is `0xF0000000` free | the loader's payload region | the loader reports it once it runs |
| MT6877 pin count | `GpioImplLib.c` | vendor `pinctrl-mt6877.c` |
| MT6877 pwrap offsets | `PmicWrapperImplLib.c` | `mtk-pmic-wrap.c` for the mt6877 compatible |
| MT6359P regulator/key masks | `MT6359PmicDxe` | vendor `mt6359-regulator.c`, `mt6359-keys.c` |

Two cheap re-checks before writing the big files, both of which would save the most work:
torvalds/linux `drivers/clk/mediatek` has **no** `clk-mt6877.c`, and
`arch/arm64/boot/dts/mediatek` has **no** `mt6877*.dtsi`. Every MT6877 address therefore
comes from the vendor tree named in `HARDWARE.md:3`, not mainline.
