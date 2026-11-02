# UEFI plan: an `mt6877Pkg` for the Bigme HiBreak Pro (MT6877)

Derived from Mu-Silicium `6b3d90b79e19` ("Version 4.1", 2026-11-01) — the two MediaTek platform packages it has
(MT6789 "G99", MT6768 "P65/G80"), `Status.md`, and HakiOS's loader (`oosexgang/hakios-xnu-kernel`,
`xnuloader/loader.c`) plus the stage-0 `XnuLoader.c` in `oosexgang/hakios-uefi`.

Paths are relative to the Mu-Silicium checkout unless prefixed with `b651/` (this directory) or `HakiOS/`.

```text
Mu-Silicium, and where a new SoC package slots in
--------------------------------------------------
Platforms/<Vendor>/<board>Pkg/      board: dsc, fdf, MemoryMapLib, KeypadDeviceLib, DeviceBuild.py
        |  !include
Silicon/MediaTek/MT6789Pkg/         SoC: clock/gpio/pmic-arb/msdc tables + SEC platform hook   <- 13 files
        |  !include
Silicon/MediaTek/MediaTekPkg/       SoC family: 5 driver INFs + 4 protocol headers (25 files, reused as-is)
        |  !include
Silicon/Silicium/SiliciumPkg/       Project-wide: SEC, BDS, GOP, heap, ACPI, DT, boot manager
```

---

## 1. File-by-file inventory of an existing MTK platform package

### 1a. SoC package — `Silicon/MediaTek/MT6789Pkg/` (13 files; MT6768Pkg is the same shape)

| Path | What it does | Scope |
|---|---|---|
| `MT6789Pkg.dsc.inc` | The whole SoC class: `USE_PHYSICAL_TIMER=1` (`:15`), `-march=armv8.2-a+crypto+rcpc` (`:20`), 8 cores / 2 clusters (`:32-33`), **GICv3 dist `0x0C000000`, redist `0x0C040000`** (`:38-39`), ACPI OEM TableId/revision (`:26-27`), SMBIOS CPU strings (`:44-45`), five library bindings (`:51,56-59`) | SoC |
| `Library/PlatformSecLib/PlatformSec.c` | The only SEC-time hook. `PlatformInitialize()` writes `WDT_MODE_KEY` to the region named `"WatchDog Timer"` (`:19-26`, `:29-32`) | SoC-ish: key is MTK-wide, address comes from the board map |
| `Library/PlatformSecLib/PlatformRegisters.h` | `WDT_MODE_KEY 0x22000000` (`:7`). MT6768's copy adds display-OVL bits (`OVL_PITCH_OFFSET`, `OVL_CONST_BLEND`) | SoC |
| `Library/PlatformSecLib/AArch64/PlatformHelper.S` | `PlatformAssemblyInitialize` is `ret` — empty (`:8-10`) | generic |
| `Library/PlatformSecLib/PlatformSecLib.inf` | Wires the two sources under `LIBRARY_CLASS = PlatformSecLib` (`:12,15,18`) | generic |
| `Library/ClockImplLib/ClockImplLib.c` | **4048 lines**: the whole MT6789 clock tree as a table of `MTK_CLOCK_DESC` — 128 entries, 185 `IFRAO_*` gates, PLL/mux/factor/gate descriptors with register offsets (`gClocks` ends at `:4046`). This is the single largest and most SoC-specific artefact | SoC |
| `Library/ClockImplLib/ClockImplLib.inf` | 16 lines, no SoC content | generic |
| `Library/GpioImplLib/GpioImplLib.c` | 11 lines: `gPlatformInfo` = pinctrl offsets + `MaxPin` (`:3-11`). The `.c` differs between the two SoCs only by `MaxPin` (179 vs 189); the `.inf` is byte-identical | SoC (one number) |
| `Library/GpioImplLib/GpioImplLib.inf` | identical across both SoCs | generic |
| `Library/PmicWrapperImplLib/PmicWrapperImplLib.c` | 13 lines: six pwrap register offsets + `ArbCapabilities`. MT6789 `InitDone2=0x0, Wacs2Cmd=0x880, ArbCapabilities=TRUE`; MT6768 `0x094/0xC20/…/FALSE` | SoC |
| `Library/MsdcImplLib/MsdcImplLib.c` | 208 lines: `MSDC_PLATFORM_INFO` (hosts, pad-tune reg, tuning steps, `AsyncFifo/BusyCheck/StopClkFix/EnhanceRx`), plus the **clock names it asks for** (`TOP_MSDC50_0`, `TOP_MSDC30_1`, `IFRAO_MSDC0_SRC/1_SRC`, `IFRAO_MSDC0/1`, `:34,57,80,94`) and the **eMMC/SD GPIO pin numbers** (`:145-164`) | SoC (pins) + board (which host is eMMC) |
| `Library/MsdcImplLib/MsdcImplLib.inf` | 28 lines; `CONSTRUCTOR = MsdcLibConstructor` (`:8`), deps on the three MTK protocols (`:23-26`), `PcdStorageIsEMMC` (`:29`) | generic |
| `Library/PmicWrapperImplLib/PmicWrapperImplLib.inf` | 16 lines | generic |

### 1b. Board package — `Platforms/Xiaomi/stonePkg/` (10 files)

| Path | What it does | Scope |
|---|---|---|
| `stone.dsc` | `PLATFORM_NAME/GUID` (`:17-18`), `OUTPUT_DIRECTORY` (`:21`), `FLASH_DEFINITION` (`:25`), `USE_CUSTOM_DISPLAY_DRIVER = 0` (`:26`), `!include MT6789Pkg/MT6789Pkg.dsc.inc` (`:28`), `PcdSystemMemoryBase 0x40000000` (`:34`), stack base/size (`:39-40`), five SMBIOS strings (`:45-49`), panel resolution `1088x2408x32` (`:54-56`), `MemoryMapLib` + `KeypadDeviceLib` bindings (`:62,67`), and only the two keypad DXEs as extra components (`:78-79`; the ACPI line `:73` is **commented out**) | board |
| `stone.fdf` | FD/FV layout: `[FD.SILICIUM_UEFI]` at `$(FD_BASE)` size `$(FD_SIZE)` (`:21-28`), `FVMAIN_COMPACT` with `Sec.inf` (`:124`), then `!include Include/APRIORI.inc`, `Include/DXE.inc`, `SiliciumPkg/Common.fdf.inc`, `MediaTekPkg/Extra.fdf.inc` (`:82-83,104,132`). Device-tree block is commented out (`:97-102`) | board |
| `Include/APRIORI.inc` | Mandatory dispatch order: DXE core, PCD, RSC/status, RuntimeDxe, CpuDxe, GIC, Metronome, Timer, then **Gpio/Clock/PmicWrapper/MT6358Pmic**, DiskIo, Partition, MsdcDxe, EmmcDxe/SdDxe, Fat, Variable, Reset, Print, DevicePath, SimpleFbDxe, keypad, consoles (`:1-48`) | board |
| `Include/DXE.inc` | The same set minus ordering, plus BdsDxe last (`:1-45`) | board |
| `Library/MemoryMapLib/MemoryMapLib.c` | **The hardware contract.** 13 DDR/reserved regions + 14 register windows, each `{Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute}` (`:5-38`). Every driver finds its block **by name** (see 1c) | board |
| `Library/MemoryMapLib/MemoryMapLib.inf` | 15 lines | generic |
| `Library/KeypadDeviceLib/KeypadDeviceLib.c` | 311 lines of generic MTK key handling (repeat/longpress state machine). The board-specific part is three lines: Vol+ = PMIC home (`:240-241`), Vol− = GPIO 66 (`:245-246`), Power = PMIC pwrkey (`:250-251`) | board (3 pins) |
| `Library/KeypadDeviceLib/KeypadDeviceLib.inf` | 16 lines | generic |
| `device.dec` | **0 bytes** — a placeholder | — |
| `DeviceBuild.py` | stuart glue: `PackagesSupported=("stonePkg")` (`:27`), scopes (`:30`), the **`PackagesPath` list that must name `Silicon/MediaTek`** (`:37`), required submodules (`:60`), `GetPlatformDscAndConfig` (`:100-101`), `SetPlatformEnv` (`:144-164`) | board |

Plus, outside the package: `Resources/Configs/stone.toml` (FD base/size + boot-image flags, `:4-6`) and
`Resources/DTBs/<device>.dtb` (only needed when `append_dtb = true`, `build_uefi.py:301-309`). **stone ships no `.dtb`**
(`Resources/DTBs/stone.*` does not exist) and sets `append_dtb = false`.

### 1c. What the shared MediaTekPkg + SiliciumPkg demand of a board

These are the constraints a new board map must satisfy; they are the real "interface".

| Consumer | Requirement | Cite |
|---|---|---|
| `Sec.c` | a region named **`DXE Heap`** — its address+length become the HOB list and the entire UEFI heap | `Silicon/Silicium/SiliciumPkg/Sec/Sec.c:62-63`, `:71-78` |
| `MemoryInitPei.c` | every `AddMem` region is sorted and **overlap-checked; overlap ⇒ `CpuDeadLoop()`**; regions below `PcdSystemMemoryBase` are treated as MMIO and skipped | `Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c:65,79-81,123` |
| `SimpleFbDxe` | a region named **`Display Reserved`**; its *base* becomes `GOP->Mode->FrameBufferBase`, size = `W*H*Bpp`. No driver programs the panel — it blits into a pre-existing buffer | `Drivers/SimpleFbDxe/SimpleFb.c:96-104,168-169,180-183` |
| `FrameBufferSerialPortLib` (DEBUG builds only) | the same `Display Reserved` region — this is the only "serial port" the MTK boards have | `Library/FrameBufferSerialPortLib/FrameBufferSerialPortLib.c:227-232`; bound at `SiliciumPkg.dsc.inc:162` |
| `GpioDxe` | `Pinctrl` | `Drivers/GpioDxe/GpioDxe.c:153` |
| `ClockDxe` | `AP Mixed`, `Top Ck Gen`, `Infra Cfg Ao` | `Drivers/ClockDxe/ClockDxe.c:629,636,643` |
| `PmicWrapperDxe` | `PMIC Wrapper` | `Drivers/PmicWrapperDxe/PmicWrapperDxe.c:137` |
| `MsdcDxe` | `MSDC-0`..`MSDC-<n-1>` and `MSDC Top-0`.., one pair per host | `Drivers/MsdcDxe/MsdcDxe.c:1396-1411` |
| `RamManagerLib` | `EMI` — DRAM geometry is read out of the EMI controller, not the map | `Library/RamManagerLib/RamManager.c:125` |
| `PlatformSecLib` | `WatchDog Timer` | `MT6789Pkg/Library/PlatformSecLib/PlatformSec.c:19` |

### 1d. What the HakiOS loader requires of the firmware

From `HakiOS/xnuloader/loader.c` and the stage-0 `XnuLoader.c` (only services that are actually called):

| Requirement | Why | Cite |
|---|---|---|
| `AllocatePages(AllocateAddress)` for the **whole 64 MiB at `0xF0000000`** | payload region + panic log; refused if the firmware reserved it (loader says so and continues) | `loader.c:18,41,4225-4229` |
| `GetMemoryMap` before `ExitBootServices` | XNU's physBase is chosen from free RAM | `loader.c:2737,2773,3477` |
| **GOP** for any visible output | banner and console are drawn by the loader; without GOP it is silent | `loader.c:4338-4351` |
| `con_out->output_string` | the loader's own text goes through the UEFI console, i.e. `GraphicsConsoleDxe` over GOP | `loader.c:187-194` |
| **`BlockIo` + `PartitionInfo` (GPT)** | log partition, UFS probe, `darwin` access | `XnuLoader.c:62-80`, `loader.c:273-290` |
| `SimpleFileSystem` (volume boot) | `\EFI\hakios\payload.bin` path | `loader.c:4187-4209` |
| `reset_system(EfiResetCold)` | every exit path | `loader.c:598,2531,4001`; `XnuLoader.c:303` |
| `PSCI` (SMC or HVC) | secondary cores; `psci-method` goes into the Apple device tree | `loader.c:3246,2291` |
| **Not required**: ACPI, device tree from firmware, USB | The loader builds its own Apple DT (`loader.c:8-9,3208-3456`) and takes USB from a *vendor* protocol it does not find on MTK (see §3) | — |

Two HakiOS-side constraints that bound the UEFI work:

- The payload must be copied to `0xF0000000` before the loader runs; HakiOS does that in `BootShim.S` behind the UEFI
  FD (`HakiOS/hakios.patch:9-29`), so the **shim is part of the boot image, not of the package**.
- The loader reaches `XnuLoaderMain()` from `PlatformBootManagerUnableToBoot()`
  (`HakiOS/hakios.patch:486`), i.e. after BDS finds nothing to boot. A firmware with a bootable volume (UFS + FAT)
  would never call it. **This matters for MT6877: if UFS ever works, HakiOS's loader must be reached another way.**

---

## 2. Ordered build list for `mt6877Pkg`

Naming: `Silicon/MediaTek/MT6877Pkg/` (SoC) + `Platforms/Bigme/b651Pkg/` (board), matching the two-package split
every MTK port uses. `26 files total: 15 copied, 11 new` — breakdown at the end of this section.

### 2.1 SoC package — `Silicon/MediaTek/MT6877Pkg/`

| # | File | Source | Why |
|---|---|---|---|
| 1 | `MT6877Pkg.dsc.inc` | **copy MT6789 and adjust** | Same structure; every value that is actually a *fact about the SoC* must be re-derived from the MT6877 tree: `PcdCoreCount 8`/`PcdClusterCount 2` (`MT6789:32-33`) stay right (6×A55 + 2×A76, `b651/HARDWARE.md:44`), GIC `0x0C000000`/`0x0C040000` stay right (`HARDWARE.md:23`), and `-march=armv8.2-a+crypto+rcpc` (`:20`) is A55/A76-correct. Change: ACPI OEM TableId/revision (`:26-27`), SMBIOS part number (`:45`). Drop or keep `PcdTimerFreqOverwrite` — unset here, and MT6877's `clock-frequency = <13000000>` (`HARDWARE.md:24`) is what `CNTFRQ` will report |
| 2 | `Library/PlatformSecLib/PlatformSec.c` | **copy MT6789, unchanged** | MT6789 and MT6768 `PlatformSec.c` differ only by MT6768's constant-blend poke; MT6877 needs only the watchdog kill. Region name comes from the board map |
| 3 | `Library/PlatformSecLib/PlatformRegisters.h` | **copy MT6789, unchanged** | `WDT_MODE_KEY 0x22000000` (`:7`) is the MTK top-rgu key; MT6877's watchdog is `toprgu@10007000` (`HARDWARE.md:62`) |
| 4 | `Library/PlatformSecLib/AArch64/PlatformHelper.S` | **copy MT6789, unchanged** | empty stub |
| 5 | `Library/PlatformSecLib/PlatformSecLib.inf` | **copy, change `FILE_GUID`** | MT6789 and MT6768 ship the *same* GUID `2C33C03B-…` — fine while only one SoC per build, wrong to inherit blindly. Same for all four others (identical GUIDs across the two SoCs) |
| 6 | `Library/ClockImplLib/ClockImplLib.c` | **must be written new** (start from MT6789's file as a template) | 4048 lines of per-SoC register tables. Nothing in it transfers verbatim: MT6789's clock *IDs* (`TOP_MSDC50_0`, `IFRAO_MSDC0_SRC`, `IFRAO_SSUSB`, `TOP_DSI_OCC`, …) are the G99's, and MT6877's gate/PLL list is a different table (the MT6789→MT6768 diff alone is 6240 changed lines). **Source of truth: `Winkmoon/android_kernel_xinran_oppo_mt6877` `drivers/clk/mediatek/clk-mt6877*.c` — port the table, do not guess.** No mainline help: `drivers/clk/mediatek` has no `clk-mt6877.c` and `arch/arm64/boot/dts/mediatek` has no `mt6877*.dtsi` (both checked against torvalds/linux today) |
| 7 | `Library/ClockImplLib/ClockImplLib.inf` | **copy, change `FILE_GUID`** | no SoC content |
| 8 | `Library/GpioImplLib/GpioImplLib.c` | **copy MT6789 and adjust one number** | Only `MaxPin` (`:10`). MT6877's pin count must come from the vendor pinctrl driver's `pin_grp` table; 189 (G99) is not a default to keep |
| 9 | `Library/GpioImplLib/GpioImplLib.inf` | **copy, change `FILE_GUID`** (same reason as #5) | generic |
| 10 | `Library/PmicWrapperImplLib/PmicWrapperImplLib.c` | **copy MT6789 and adjust** | MT6789 and MT6768 differ here (`Wacs2Cmd 0x880` vs `0xC20`, `ArbCapabilities TRUE` vs `FALSE`) — proof this is per-SoC. MT6877's pwrap is at `0x10026000` (`HARDWARE.md:27`); the register set must be read from `drivers/soc/mediatek/mtk-pmic-wrap.c` for the MT6877 compatible |
| 11 | `Library/PmicWrapperImplLib/PmicWrapperImplLib.inf` | **copy, change `FILE_GUID`** | generic |
| 12 | `Library/MsdcImplLib/MsdcImplLib.c` | **must be written new** (copy as skeleton) | Two reasons. (a) The eMMC/SD pin numbers (`:145-164`) are board wiring. (b) **More important: MT6877 storage is UFS**, not eMMC/SD (`HARDWARE.md:32-35`), so this library has nothing to talk to. Keep it compiled (it is referenced unconditionally by `MediaTekPkg.dsc.inc:37`) but expect it to fail to find `MSDC-0` and skip |
| 13 | `Library/MsdcImplLib/MsdcImplLib.inf` | **copy, change `FILE_GUID`** | generic |

### 2.2 Board package — `Platforms/Bigme/b651Pkg/`

| # | File | Source | Why |
|---|---|---|---|
| 14 | `b651.dsc` | **copy stone and adjust** | Same skeleton. Changes: `PLATFORM_NAME/GUID`, `OUTPUT_DIRECTORY`, `!include MT6877Pkg/MT6877Pkg.dsc.inc` (`stone.dsc:28`), SMBIOS strings (`:45-49`), `MemoryMapLib`/`KeypadDeviceLib` paths (`:62,67`), and `PcdFrameBuffer*` (`:54-56`) |
| 15 | `b651.fdf` | **copy stone.fdf** | Identical FD/FV skeleton; only the `!include` name changes. Keep the DT block commented out (`stone.fdf:97-102`) — the loader builds its own DT |
| 16 | `Include/APRIORI.inc` | **copy stone, then delete** the MTK driver lines that cannot work yet | Start from `stone/Include/APRIORI.inc:13-16` (Gpio/Clock/PmicWrapper/MT6358Pmic) and `:21-26` (Msdc/Emmc/Sd/Fat). For milestone 1 (§4) **clock+gpio+pwrap+pmic must stay** — GOP needs `Display Reserved` only, but SEC needs the watchdog poke through the map, and the keypad needs the GPIO/PMIC protocols |
| 17 | `Include/DXE.inc` | **copy stone, same trimming** | `stone/Include/DXE.inc:29-37` |
| 18 | `Library/MemoryMapLib/MemoryMapLib.c` | **must be written new** | The single highest-value new file: 13+14 entries, every one an MT6877 address. Usable today, all `SoC`-class and verified in the vendor DT: GIC `0x0C000000`/`0x0C040000` (`HARDWARE.md:23`), watchdog `0x10007000` (`:62`), pwrap `0x10026000` (`:27`), UFS `0x11270000` (`:32`), thermal `0x1100b000` (`:36`), dispsys `0x14000000` + OVL `0x14005000` (`:59,61`), UART0 `0x11002000` (`:13`). **Unknown and blocking: the DRAM/reserved layout** — the vendor map is a 998 MiB stub (`HARDWARE.md:52-53`) and the real one is in the board's own DT. Reserved regions (`SSMR CMA 0xc0000000+256M`, `mcupm_share`, `sspm/scp/adsp_share`, `consys`) must be carried over or the firmware will hand XNU memory the modem is using |
| 19 | `Library/MemoryMapLib/MemoryMapLib.inf` | **copy** | generic |
| 20 | `Library/KeypadDeviceLib/KeypadDeviceLib.c` | **copy stone, change 3 lines** | Generic state machine is reusable verbatim; pins (`stone:240-251`) are not. **MT6877 keys are on MT6359P through pwrap — NOT PMIF** (`HARDWARE.md:31`: "no PMIF key/pwrkey node"). So the `KEY_TYPE_PMIC` path works only if MT6359P's TOPSTATUS is implemented (see #23) |
| 21 | `Library/KeypadDeviceLib/KeypadDeviceLib.inf` | **copy** | generic |
| 22 | `b651.dec` | **copy stone.dec** | 0-byte placeholder |
| 23 | `DeviceBuild.py` | **copy stone and adjust** | `PackagesSupported`, `Scopes`, `GetPlatformDscAndConfig` (`stone:100-101`), `SetPlatformEnv` (`:147-148`). `PackagesPath` needs `Silicon/MediaTek`, `Silicium-ACPI`, `Silicium-ACPI/Platforms/<Vendor>`, `Silicium-ACPI/Silicon/MediaTek` (`stone:32-42`) |
| 24 | `Drivers/MT6359PmicDxe/` (`.c` + `.inf`) | **must be written new** (MT6358PmicDxe as template) | **The PMIC is a different chip.** `MediaTekPkg` ships only `MT6358PmicDxe` (`MediaTekPkg.dsc.inc:36`); grepping the whole tree for `MT6359` returns **zero** hits. MT6877 uses MT6359P (`HARDWARE.md:26`). Every register offset in `MT6358PmicDxe.c:9-13,63-95` is MT6358's. Regulators (`ldo_vemc`, `ldo_vmch`, `ldo_vmc`) and the pwrkey/homekey masks must be re-derived |
| 25 | `Resources/Configs/b651.toml` (not a package file, but required) | **copy stone.toml and adjust** | `build_uefi.py:115-125` refuses to build without it. `[uefi_fd] base/size` must match where the bootloader will place the FD |

### 2.3 Counts

| Category | Count | Files |
|---|---|---|
| Copy unchanged | 6 | PlatformSec.c, PlatformRegisters.h, PlatformHelper.S, KeypadDeviceLib.inf, b651.dec, b651.fdf |
| Copy + change GUID only | 5 | PlatformSecLib.inf, ClockImplLib.inf, GpioImplLib.inf, PmicWrapperImplLib.inf, MsdcImplLib.inf |
| Copy + adjust values | 9 | MT6877Pkg.dsc.inc, GpioImplLib.c, PmicWrapperImplLib.c, MsdcImplLib.c, b651.dsc, APRIORI.inc, DXE.inc, KeypadDeviceLib.c, DeviceBuild.py |
| **Must be written new** | **5** | ClockImplLib.c, MemoryMapLib.c, MT6359PmicDxe.c, MT6359PmicDxe.inf, `Resources/Configs/b651.toml` |
| **Must be written new, no template at all** | **2** | `UfsDxe` equivalent, MTK USB-device equivalent (both out of milestone 1; see §3) |

**Headline: 20 files copied-or-adjusted (14 of them near-verbatim), 5 written new for milestone 1** (the two large
libs — ClockImplLib.c at ~4000 lines and MemoryMapLib.c — dominate the effort). With UFS and USB: **+2 new driver
trees, ~10 files.**

---

## 3. Blockers

### 3.1 USB device mode: it does not exist for MediaTek, and the one place it does exist is Qualcomm firmware

This is the load-bearing question, so the evidence is laid out in full.

**In Mu-Silicium's MediaTek code: zero device-mode code.**

```text
grep -rniE "usb" Silicon/MediaTek Platforms/Xiaomi/stonePkg Platforms/Xiaomi/lancelotPkg
                 --include=*.c --include=*.h --include=*.inf --include=*.inc --include=*.dsc
  -> 63 hits, in exactly 3 files, and NOT ONE is a driver:
       59  ClockImplLib.c (MT6789 + MT6768): clock-tree names only
           (APMIXED_USBPLL :23, TOP_USB_SEL :1692, TOP_USB :3026, IFRAO_SSUSB :3669)
        4  MT6358PmicDxe.c: the regulator "ldo_vusb" and its voltage table (:86,415,423-424)
  -> 0 driver INFs, 0 protocols, 0 PCDs
grep -rn "SSUSB|ssusb|mtu3|xhci_mtk" --include=*.c --include=*.h --include=*.inf   -> 0 hits
MediaTekPkg.dsc.inc:29-37  lists 5 components: GpioDxe, ClockDxe, PmicWrapperDxe, MT6358PmicDxe, MsdcDxe. No USB.
MediaTekPkg.dec:13-17      declares 4 protocols: Gpio, PmicWrapper, Pmic, Clock. No USB.
```

**In `Status.md`: the only two MediaTek devices are both ❌ for both directions.**

| Device | SoC | USB Host | USB Device | line |
|---|---|---|---|---|
| Xiaomi POCO M5 (`stone`) | MT6789 | ❌ | ❌ | `Status.md:582-583` |
| Xiaomi Redmi 9 (`lancelot`) | MT6768 | ❌ | ❌ | `Status.md:619-620` |

Machine-checked: parsing the `MediaTek Devices` section (`Status.md:554-675`) finds exactly two `USB Device Mode` rows
and **neither contains ✅**. Whole-file grep shows ✅ entries exist elsewhere (e.g. `:122`, `:704`, `:734`) — all in
Qualcomm sections.

**Where the ✅ ones come from — and why it cannot be reused:**

```text
Platforms/OnePlus/infinitiPkg/Include/DXE.inc:94   INF Binaries/infiniti/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
                                            :102   INF Binaries/infiniti/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
                                            :103   INF Binaries/infiniti/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
                                            :104   INF Binaries/infiniti/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
                                            :105   INF Binaries/infiniti/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
                                            :96    INF Binaries/infiniti/QcomPkg/Drivers/XhciDxe/XhciDxe.inf
```

Every one is a **prebuilt Qualcomm binary** under `Binaries/<device>/QcomPkg/…` (the `Binaries` submodule,
`.gitmodules:1-3`), written for Qualcomm's DWC3 controller and Qualcomm's `UsbDeviceProtocol`. Nothing in
`Silicon/` implements device mode from source: the only `UfsDxe`/`XhciDxe` sources in the tree are Samsung's, and
`UfsDxe` needs `PlatformUfsLib` + `UfsCalAdapterLib` bound to a Samsung board (`.inf:22-23`).

**And HakiOS's loader needs exactly that Qualcomm protocol.** `HakiOS/xnuloader/loader.c:3656` binds
`QCOM_USB_DEVICE_GUID`, whose definition is literally commented as Qualcomm's
(`HakiOS/xnuloader/efi.h:246-247`: "ABL, QcomModulePkg/Include/Protocol/EFIUsbDevice.h … The stock fastboot is built on
it", GUID at `efi.h:285`). On anything else `usb_start()` prints `usb: device protocol not found` and returns
(`loader.c:3669-3670`), and the loader falls through to one-shot mode (`loader.c:4359-4367`).

**Consequence for this port — this is the honest blocker, and it is worse than "the UEFI will not have USB":**

| Path | Needs | Does it work on MT6877 today? |
|---|---|---|
| `xl ramdisk` (push the 2.5 GB system image over USB after boot) | HakiOS's USB command loop | **No.** Requires `QCOM_USB_DEVICE_GUID`; MT6877 firmware cannot provide it without a protocol-compatible MTK driver |
| `fastboot boot` of a HakiOS boot image | LK-side `fastboot boot`, not the firmware | **Unknown / likely no.** `b651/SCOPE.md:49-50` records it as "frequently refused or hangs" on MTK LK. Nothing in Mu-Silicium fixes this — the BootShim is appended to the image, the decision is LK's |
| `fastboot flash boot` then reboot | only LK's flash path | **The only path with evidence.** `b651/GATE1.md:93-131` makes it gate 1; requires `seccfg` unlocked, proven by reading the partition, not by `fastboot oem device-info` |
| Boot from volume (`\EFI\hakios\payload.bin`) | UFS + FAT + a bootable option | **No** — no MTK UFS driver exists (§3.2), so BDS has no volume to read |

So: **a `mt6877Pkg` today can only be delivered by writing an image to a partition and rebooting.** It cannot load a
system over USB, and it cannot read one off the phone's own storage. The payload must ride inside the boot image.

### 3.2 Storage: MT6877 is UFS-only; Mu-Silicium has no MediaTek UFS driver

`MediaTekPkg.dsc.inc:37` adds `MsdcDxe`, and `MsdcDxe.inf:31` publishes only `gEfiSdMmcPassThruProtocolGuid` — an
eMMC/SD controller driver. MT6877's storage is UFS 2.2 on `ufshci@11270000` (`b651/HARDWARE.md:32-35`); the vendor DT for the sibling OPPO board
has no mmc controller in use, while `lancelot.dts` has 8 `mmc` hits because that SoC *is* eMMC.
Mu-Silicium has no `UfsDxe` under `Silicon/MediaTek` (whole-tree search finds Samsung's only). Consequences:

- No BlockIo, therefore no `PartitionInfo`, therefore HakiOS's `FindPartition("logdump")` returns NULL
  (`XnuLoader.c:62-91`) and the loader writes no log — silent unless the panel shows the banner.
- No `SimpleFileSystem`, therefore the volume-payload path is dead (`loader.c:4197-4200` prints "the loader was not
  started from a volume with a file system").
- `Logo`/`NoBootOS` BMPs come from the FV, not storage, so the **visible** boot still works.

Note the vendor Linux driver is `mediatek,mt8183-ufshci` (`HARDWARE.md:33`) — an *open* Linux driver, and DWC UFSHCI
3.x like `ufs-qcom` (`HARDWARE.md:89`). A port is plausible but is a new driver, not a copy.

### 3.3 Display: there is no framebuffer to point GOP at

`SimpleFbDxe` does not drive a display — it registers a GOP over a `Display Reserved` region that something else
already configured (`SimpleFb.c:96-104`). On the MTK ports that "something else" is the bootloader's continuous-splash
(no source in Mu-Silicium programs DSI: `grep -i "dsi|dpi|mipi" Silicon/MediaTek` returns only clock-tree names).

On the HiBreak Pro the panel is **e-ink**, and `b651/SCOPE.md:74-80` records that no EPDC driver exists in HakiOS, in
Mu-Silicium or in XNU, and `HARDWARE.md:57-58` that the MT6877 vendor trees have **no epdc/eink node at all** (the
`extd_epd.c` variants exist only for mt6765/6768/6779/6785/6833/6885). Two unknowns stack here:

1. Whether the HiBreak Pro's *own* bootloader leaves a filled framebuffer anywhere (the OPPO board passes
   `atag,videolfb-fb_base_l = <0x7e605000>`, `HARDWARE.md:64` — a *hint*, on a different board).
2. Whether e-ink can serve as a GOP at all: macOS/Darwin expects a video-rate surface; a text console is the realistic
   target (`SCOPE.md:80`).

**Therefore: the very first milestone cannot be "shows the boot logo". The only reliable first output is UART.**

### 3.4 UART: available in hardware, but not wired into Mu-Silicium for MTK

Good news: `HARDWARE.md:13-15` gives UART0 at `0x11002000`, `GIC_SPI 141`, `mediatek,mt6577-uart`, and Linux's
`8250_mtk.c` shows the IP is a 16550 with `regshift = 2`, `UPIO_MEM32` (`:565-566,691-692`), plus an MTK-only
highspeed register `MTK_UART_HIGHS = 0x09` that must be written `0x3` for any baud ≥ 115200 (`:28,366-371`).

Bad news: Mu-Silicium binds **no** real serial port for any MTK board. `SerialPortLib` is
`FrameBufferSerialPortLib` in DEBUG and `BaseSerialPortLibNull` in RELEASE (`SiliciumPkg.dsc.inc:161-167`), and
**no platform overrides it** (`grep -rn "SerialPortLib|" Platforms/` → 0 hits). So DEBUG output goes to the panel; with
no panel, DEBUG output goes nowhere.

What is needed: bind `MdeModulePkg/Library/BaseSerialPortLib16550` with `PcdSerialUseMmio=TRUE`,
`PcdSerialRegisterBase=0x11002000`, `PcdSerialRegisterStride=4` (register stride = `1 << regshift`),
`PcdSerialRegisterAccessWidth=32`, and an MTK shim that writes `MTK_UART_HIGHS=3` at init (EDK2's 16550 lib does not
know that register). Note `PcdSerialRegisterBase` is `UINT64` in MdeModulePkg.dec and the 16550 lib is
`MODULE_TYPE = BASE`, so it works in SEC. **Caveat: the pinmux for UART0 on this board is unknown** (`SCOPE.md:66`
lists "UART pins" as an open question) — if the bootloader does not leave UART0 muxed and clocked, this prints
nothing either.

### 3.5 Other blockers, stated briefly

| # | Blocker | Evidence | Blocks |
|---|---|---|---|
| 1 | No payload region reservation check | `0xF0000000` is inside the DDR window; HakiOS says "whether Silicium leaves `0xF0000000` free … is not known" (`HakiOS/docs/uefi-boot.md:62`). The loader handles refusal gracefully (`loader.c:4226-4229`) but silently | volume-boot path |
| 2 | Boot manager reaches the loader only via "nothing to boot" | `hakios.patch:486` puts `XnuLoaderMain()` in `PlatformBootManagerUnableToBoot`. If a FAT volume ever appears, BDS boots it instead | all paths once storage works |
| 3 | Signing / `seccfg` | `SCOPE.md:47-54`, `GATE1.md:115-131` | everything |
| 4 | No MT6877 device tree for the board | `HARDWARE.md:106-107`: only the phone's own `dtb`/`/proc/device-tree` answers e-ink base, DRAM size, UART pinmux, PMIC rails | memory map + display |
| 5 | `Binaries/` is a submodule and holds **no MediaTek vendor code** | `Device-Binaries` tree listing: 80 entries, all Qualcomm/Samsung codenames; no `mt67xx`. So the "extract vendor DXEs from your own xbl" route (`HakiOS/README.md`) yields MTK drivers of unknown interface | alternative to writing drivers |

---

## 4. Minimum first milestone

**Goal: one line of text out of the phone.** No display, no storage, no operating system.

```text
milestone 1 = BootShim -> Sec -> UEFI print -> UART0 @0x11002000 -> a serial adapter
```

Why UART and not the screen: the panel is e-ink with no driver anywhere (§3.3), and `PrintFirmwareVersion()` already
prints the product line from SMBIOS PCDs (`Sec.c:40-50`) — it just needs a `SerialPortLib` that is not the null one.

### The smallest build

| Part | Contents | New code |
|---|---|---|
| `MT6877Pkg.dsc.inc` | copy MT6789, keep `USE_PHYSICAL_TIMER=1` and the two GIC PCDs, set `PcdCoreCount 8`/`PcdClusterCount 2` | no |
| `PlatformSecLib` | 4 files copied | no |
| `ClockImplLib` | **stub only** — `gClocks[] = {}`, `gClockCount = 0`. Nothing in SEC or in the serial path reads it | ~10 lines |
| `GpioImplLib` / `PmicWrapperImplLib` / `MsdcImplLib` | stubs (`gPlatformInfo` zeroed) — their DXEs are *not* in the FV | ~10 lines each |
| `MemoryMapLib.c` | **6 entries only**: `UEFI FD`, `UEFI Stack`, `DXE Heap` (required by `Sec.c:62-63`), `Watchdog Timer`, `Pmic Wrapper` (not needed yet), `Display Reserved` — the last one **faked to a dummy buffer in DXE heap** so `SimpleFbDxe` and `FrameBufferSerialPortLib` do not abort. Sizes chosen by hand: FD 2 MiB, stack 256 KiB, heap 60 MiB, all below the first reserved region | yes, ~60 lines |
| `b651.dsc` / `.fdf` / `APRIORI.inc` / `DXE.inc` | copied, then **all components from `stone/Include/DXE.inc:29-37` removed** — no Gpio/Clock/PmicWrapper/Msdc/Emmc/Sd/Fat | trim only |
| serial | `SerialPortLib|MdeModulePkg/Library/BaseSerialPortLib16550/...` + a 3-line `MtkUartInit()` that writes `0x3` to `0x11002000 + 0x09*4` | yes, ~20 lines |
| `b651.toml` | FD base/size, `append_dtb = false`, `boot_image_kernel.kernel_compression = "gzip"` | no |

### What prints, and in what order

```text
BootShim.S            copies the FD from its load address to FD_BASE            (no output)
Sec.c:98  PlatformInitialize()   writes WDT_MODE_KEY to "WatchDog Timer"        (no output)
Sec.c:100 SerialPortInitialize() -> MTK 16550 shim writes MTK_UART_HIGHS=3      (no output)
Sec.c:47  "Project Silicium for <PcdSmbiosSystemManufacturer> <...Model>"
Sec.c:48  "Firmware Version ... Build at ..."                                   <- first evidence of life
Sec.c:116 InitializeMemory()     -> MemoryInitPeiLib validates the map
                                    (overlap or missing "DXE Heap" => CpuDeadLoop, silent)
DXE:      GraphicsConsoleDxe -> SimpleFbDxe -> GOP over the dummy buffer
          (on the panel: nothing; on UART: nothing, GOP is memory only)
BDS:      MsBootPolicy -> PlatformBootManagerUnableToBoot -> XnuLoaderMain()
          (only if HakiOS's patch is applied; otherwise a "no OS" logo + shutdown)
```

Accepting criteria, in order — each is a separate experiment, do not merge them:

1. `SILICIUM_UEFI.fd` **builds** at all (`python3 build_uefi.py -d b651 -r DEBUG`). This proves the package graph,
   not the phone.
2. The three `Sec.c` lines appear on a **UART** adapter at 921600 or 115200 8N1. This proves `FD_BASE`, the watchdog
   key, the UART pinmux and the DDR location the bootloader used.
3. The same lines appear when the image is **flash-booted** from a boot partition (gate 1 passed first).
4. Only then: add `Display Reserved` for real and try to see the logo — which **requires gate 3 (e-ink)** first.

### What NOT to include in milestone 1

`ClockDxe`, `GpioDxe`, `PmicWrapperDxe`, `MT6359PmicDxe`, `MsdcDxe`, `EmmcDxe`, `SdDxe`, `Fat`, keypad — every one
either needs a table that must be re-derived from the vendor kernel (clock: 4048 lines; pwrap: register set) or has
nothing to bind to (storage). They are added back one at a time, after step 2 above passes, in the order:
`Gpio+Clock` (unblocks everything) → `PmicWrapper+MT6359Pmic` (unblocks keys and storage regulators) → storage.

### Estimated order of magnitude

| Work | Size |
|---|---|
| Milestone 1 skeleton (all of the table above) | ~150 lines of new code, 2 days of iteration once a phone is on the desk |
| `ClockImplLib.c` for real | the bulk: ~1000–4000 lines of table ported from `clk-mt6877*.c` |
| `MT6359PmicDxe` | ~600 lines (MT6358PmicDxe is 580; the regulator tables are the work) |
| MTK `UfsDxe` | new driver, plausibly 2–4 weeks (DWC UFSHCI 3.x + MPHY at `0x11fa0000` + `refclk_ctrl=2`) |
| MTK USB device mode | new driver **and** a shim exposing `QCOM_USB_DEVICE_GUID`, or a change to HakiOS's loader to speak a neutral protocol. Not in this plan's scope |
| e-ink GOP | out of scope entirely (`SCOPE.md:72-80`) |

---

## 5. What is unknown, and would change this plan

| Unknown | Why it matters | Where it would come from |
|---|---|---|
| The HiBreak Pro's own device tree | real DRAM map, reserved regions, UART pinmux, e-ink base, PMIC rails | `adb pull /sys/firmware/fdt` (`GATE1.md:27-48`) |
| Whether LK accepts `fastboot boot` | decides RAM-only vs flash-only | `GATE1.md:70-91` |
| Whether the bootloader leaves UART0 muxed | decides whether milestone 1 is even observable | phone, serial adapter |
| Whether `0xF0000000` is free | volume-boot path | the loader reports it, once it runs |
| MT6877 pinctrl pin count | `GpioImplLib.c:10` | vendor `pinctrl-mt6877.c` |
| MT6877 pwrap register offsets | `PmicWrapperImplLib.c:4-11` | `mtk-pmic-wrap.c` for the mt6877 compatible |
| MT6359P regulator/key masks | `MT6359PmicDxe` | vendor `mt6359-regulator.c` / `mt6359-keys.c` |
| Whether any MTK USB-device protocol can be made `QCOM_USB_DEVICE_GUID`-compatible | whether `xl ramdisk` can ever work on this phone | design decision, not a lookup |

Two facts to re-check before writing code, because both are cheap and both would save the most work:
**torvalds/linux `drivers/clk/mediatek` has no `clk-mt6877.c` and `arch/arm64/boot/dts/mediatek` has no `mt6877*.dtsi`**
(checked today via the GitHub contents API) — so every MT6877 address must come from the vendor tree named in
`b651/HARDWARE.md:3`, not from mainline.

