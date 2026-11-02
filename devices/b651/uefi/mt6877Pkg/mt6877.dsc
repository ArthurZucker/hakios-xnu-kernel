## @file
#  Platform description for the MediaTek MT6877 (Bigme HiBreak Pro, b651).
#
#  SCAFFOLD ONLY -- THIS FILE HAS NEVER BEEN PARSED OR BUILT.
#
#  There was no Mu-Silicium checkout and no edk2 tree on hand when this was
#  written. The syntax is the edk2 DSC format as described by UEFI-PLAN.md's
#  reading of stone.dsc (UEFI-PLAN.md:47) and MT6789Pkg.dsc.inc (UEFI-PLAN.md:29),
#  but nothing has validated it. Expect to fix stanzas; treat the STRUCTURE and the
#  TODO list as the deliverable, not the text.
#
#  Shape: this is a FLATTENED DSC. In a real Mu-Silicium tree the split is
#      Platforms/Bigme/b651Pkg/b651.dsc          (board: FLASH_DEFINITION, SMBIOS,
#                                                 panel, MemoryMapLib binding)
#          !include MT6877Pkg/MT6877Pkg.dsc.inc  (SoC: GIC, cores, -march, libs)
#  Both are collapsed here because only the SoC half was in scope for this
#  scaffold. The comment markers say which stanza belongs where.
#
#  What is deliberately ABSENT and why -- see PLAN.md for the full list:
#    * every MdeModulePkg / MdePkg / SiliciumPkg component and library binding.
#      A real port takes them from SiliciumPkg/SiliciumPkg.dsc, which this
#      scaffold does not have; restating them from memory would be inventing
#      paths.
#    * b651.fdf. FLASH_DEFINITION points at it; it does not exist yet.
#    * Clock/Gpio/PmicWrapper/Msdc ImplLibs, PlatformSecLib, MT6359PmicDxe, the
#      UFS driver and the serial shim. Each is a commented-out block with a TODO.
##

[Defines]
  PLATFORM_NAME                  = b651
  PLATFORM_GUID                  = 31B6351C-36CC-4529-8A16-5150A299E332
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/b651
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

  # TODO: b651.fdf does not exist. It is step 21 in PLAN.md and is a copy of
  #       stone.fdf with only the !include name changed.
  FLASH_DEFINITION               = b651Pkg/b651.fdf

  # Board: from stone.dsc:26. Irrelevant on this phone (the panel is e-ink and
  # nothing here drives it) but it gates which display driver the FV takes.
  DEFINE USE_CUSTOM_DISPLAY_DRIVER = 0

#
# ---------------------------------------------------------------------------
# SoC class  (in a real tree: MT6877Pkg.dsc.inc)
# ---------------------------------------------------------------------------
[BuildOptions]
  #
  # From MT6789Pkg.dsc.inc:20 (UEFI-PLAN.md:29). The same string is right for
  # MT6877: 6x Cortex-A55 + 2x Cortex-A76 (HARDWARE.md:44), both armv8.2-a with
  # crypto and rcpc.
  #
  GCC:*_*_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc

  #
  # TODO: Mu-Silicium's DSC carries a common flag block (stack protector,
  #       -mstrict-align, etc.) that is shared by every platform. It is NOT
  #       reproduced here because its exact contents were not read. Copy it from
  #       MT6789Pkg.dsc.inc:14-21 before building, or SEC will not link.
  #

#
# ---------------------------------------------------------------------------
# PCDs -- SoC class
# ---------------------------------------------------------------------------
[PcdsFixedAtBuild]
  #
  # From MT6789Pkg.dsc.inc:32-33 (UEFI-PLAN.md:29): 6x A55 + 2x A76 = 8 cores,
  # 2 clusters. HARDWARE.md:44 confirms the topology for MT6877.
  #
  gArmTokenSpaceGuid.PcdCoreCount|8
  gArmTokenSpaceGuid.PcdClusterCount|2

  #
  # GICv3: distributor 0x0C000000, redistributor 0x0C040000.
  # HARDWARE.md:23 (mt6877.dts:700-711) -- redist +0x200000, one region.
  #
  # These tokens are NOT memory-map entries (which live in MemoryMapLib.c); they
  # configure ArmGicDxe. TODO: confirm the token NAMES against the ArmPkg DEC used
  #       by this Mu-Silicium revision -- ArmGicDxe's redistributor PCD has been
  #       renamed across edk2 releases.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C040000
  # TODO: PcdGicRedistributorLength / frame-count equivalent -- the +0x200000
  #       span has to be expressed somewhere and this scaffold does not know the
  #       token name.

  #
  # Generic timer. MT6789 sets USE_PHYSICAL_TIMER=1 (UEFI-PLAN.md:29); MT6877's
  # DT says clock-frequency = <13000000> (HARDWARE.md:24), so CNTFRQ will report
  # 13 MHz and PcdTimerFreqOverwrite should stay unset.
  #
  # DEFINE USE_PHYSICAL_TIMER = 1
  #
  # TODO: the DEFINE above is commented out because no consumer of it in
  #       SiliciumPkg is present in this scaffold. Restore it when the real
  #       SiliciumPkg DSC is available -- the timer library keys off it.

  #
  # Serial -- THIS IS THE MILESTONE-1 OUTPUT PATH (UEFI-PLAN.md:271-278).
  #
  # UART0 is a 16550 at 0x11002000 with regshift = 2, i.e. a 4-byte stride and
  # 32-bit MMIO (HARDWARE.md:13,18; 8250_mtk.c UPIO_MEM32).
  #
  # 921600 8N1 is the board's console baud (HARDWARE.md:16). NOTE the MTK
  # high-speed register: at any baud >= 115200 the IP needs MTK_UART_HIGHS = 0x3
  # written at offset 0x09 (HARDWARE.md:19). edk2's BaseSerialPortLib16550 does
  # not know that register, so a ~3-line shim has to write it at init, or the
  # output is garbage. The shim is NOT in this scaffold.
  #
  # TODO: the exact 16550 PCD set is unverified; the four below are the ones
  #       UEFI-PLAN.md:271-274 names. Confirm against MdeModulePkg.dec.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseMmio|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x11002000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterStride|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterAccessWidth|32
  #
  # TODO: PcdSerialBaudRate has no 921600 encoding in the 16550 library's
  #       standard set in some edk2 revisions. If it must be 115200, say so here
  #       rather than silently getting 115200 and blaming the pinmux.
  #

  #
  # System memory base. SoC fact: the vendor DT's memory node starts at
  # 0x40000000 (HARDWARE.md:52). The SIZE is not set because it has never been
  # read off this phone (HARDWARE.md:53).
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  # TODO: PcdSystemMemorySize -- see MemoryMapLib.c and PLAN.md section 6.

  #
  # This package's own map PCDs (declared in mt6877.dec). PLACEHOLDERS.
  #
  gMT6877TokenSpaceGuid.PcdUefiFdBase|0x00000000
  gMT6877TokenSpaceGuid.PcdUefiFdSize|0x00200000
  gMT6877TokenSpaceGuid.PcdUefiStackSize|0x00040000
  gMT6877TokenSpaceGuid.PcdDxeHeapSize|0x03C00000

#
# ---------------------------------------------------------------------------
# PCDs -- board class  (in a real tree: b651.dsc)
# ---------------------------------------------------------------------------
[PcdsFixedAtBuild.AARCH64]
  #
  # From stone.dsc:54-56 (UEFI-PLAN.md:47): panel resolution as W x H x Bpp.
  # Kept only because SimpleFbDxe reads it for the GOP mode; on an e-ink panel
  # this is a placeholder, not the panel. The HiBreak Pro's real geometry is in
  # its /eink node (PHONE.md:57-64) whose values need root to read.
  #
  # TODO: replace with the HiBreak Pro's eink_fb_width/eink_fb_height/Bpp.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|1088
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|2408
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoPixelBpp|32

  #
  # SMBIOS strings -- what Sec.c:40-50 prints, i.e. THE FIRST TEXT TO LEAVE THE
  # PHONE. From stone.dsc:45-49 (UEFI-PLAN.md:47).
  #
  # TODO: these are the b651 values as far as they are known (PHONE.md:11-20).
  #       The model string on this unit is `HiBreak`, not `hibreak_eea`; the
  #       vendor is Bigme; the part number is unverified.
  #
  # gSiliciumTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Bigme"
  # gSiliciumTokenSpaceGuid.PcdSmbiosSystemProductName|"HiBreak Pro"
  # gSiliciumTokenSpaceGuid.PcdSmbiosSystemVersion|"B651"
  # gSiliciumTokenSpaceGuid.PcdSmbiosBoardProduct|"b651"
  # gSiliciumTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"MT6877"
  #
  # TODO: commented out because gSiliciumTokenSpaceGuid is declared in a package
  #       that is not present in this scaffold (SiliciumPkg.dec). Uncomment once
  #       the real tree is in place and the token names are confirmed.
  #

#
# ---------------------------------------------------------------------------
# Library bindings
# ---------------------------------------------------------------------------
[LibraryClasses]
  #
  # --- SILICIUM-WIDE: NOT RESTATED ---
  # Sec, Pei, Dxe, Uefi, ArmLib, PcdLib, DebugLib, HobLib, ArmGicLib, TimerLib,
  # MemoryInitPeiLib, BdsLib, ... every one is bound by
  # SiliciumPkg/SiliciumPkg.dsc. A platform DSC that redeclares them without the
  # real values is worse than one that omits them.
  #
  # --- THIS PACKAGE ---
  # "MT6877Pkg/MT6877Pkg.inf" as the LIBRARY_CLASS name from MemoryMapLib.inf.
  # TODO: fix the class name and the path to match the real tree.
  #
  # MemoryMapLib|MT6877Pkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # --- SERIAL: THE MILESTONE-1 BINDING ---
  # Mu-Silicium binds SerialPortLib to FrameBufferSerialPortLib in DEBUG and
  # BaseSerialPortLibNull in RELEASE (SiliciumPkg.dsc.inc:161-167); no platform
  # overrides it (UEFI-PLAN.md:267-269). So on MTK, DEBUG output goes to a panel
  # and with no panel it goes nowhere.
  #
  # TODO: this must WIN over SiliciumPkg's binding, which a platform [LibraryClasses]
  #       stanza does in edk2 DSC precedence -- verify that against the real tree.
  #       It is the single line that makes milestone 1 observable.
  #
  # SerialPortLib|MdeModulePkg/Library/BaseSerialPortLib16550/BaseSerialPortLib16550.inf

  #
  # --- MILESTONE-1 STUBS: NOT WRITTEN ---
  # TODO: ClockImplLib / GpioImplLib / PmicWrapperImplLib / MsdcImplLib stubs
  #       (gClocks[] = {}, gPlatformInfo zeroed). Their DXEs are NOT in the FV at
  #       milestone 1, so the bindings can be absent; add them with the DXEs.
  #
  # ClockImplLib|MT6877Pkg/Library/ClockImplLib/ClockImplLib.inf
  # GpioImplLib|MT6877Pkg/Library/GpioImplLib/GpioImplLib.inf
  # PmicWrapperImplLib|MT6877Pkg/Library/PmicWrapperImplLib/PmicWrapperImplLib.inf
  # MsdcImplLib|MT6877Pkg/Library/MsdcImplLib/MsdcImplLib.inf
  #
  # TODO: PlatformSecLib. REQUIRED for milestone 1 -- it is the watchdog poke, and
  #       without it the phone reboots before any text is printed.
  # PlatformSecLib|MT6877Pkg/Library/PlatformSecLib/PlatformSecLib.inf

#
# ---------------------------------------------------------------------------
# Components
#
#  Milestone 1 ships a deliberately short list. Everything storage-, display- or
#  keypad-related is commented out with the reason: on this SoC those drivers have
#  nothing to bind to or a table that must be re-derived from the vendor kernel.
# ---------------------------------------------------------------------------
[Components]
  #
  # --- SILICIUM-WIDE: NOT RESTATED ---
  # Sec, DXE core, PCD, RuntimeDxe, CpuDxe, ArmGicDxe, Timer, Reset,
  # DevicePath, Print, Variable, Bds, GraphicsConsole, SimpleFb ... all come from
  # SiliciumPkg. This scaffold does not know their exact INF paths.
  #

  # --- THIS PACKAGE ---
  # MT6877Pkg/Library/MemoryMapLib/MemoryMapLib.inf   # library, bound above

  #
  # --- THE ONE COMPONENT THAT MATTERS FOR MILESTONE 1'S OUTPUT ---
  # TODO: the MTK 16550 shim (writes MTK_UART_HIGHS = 0x3). ~20 lines, no file
  #       yet. It is not a component so much as a library or a Sec hook; decide
  #       which against the real Sec.c.
  # MT6877Pkg/Library/MtkUartLib/MtkUartLib.inf

  #
  # --- OUT OF MILESTONE 1, LISTED SO THE GAP IS EXPLICIT ---
  #
  # GpioDxe / ClockDxe / PmicWrapperDxe -- need "Pinctrl", "AP Mixed", "Top Ck
  #   Gen", "Infra Cfg Ao", "Pmic Wrapper" from the map (three of those five are
  #   TODO in MemoryMapLib.c) and ClockImplLib's table.
  # MT6359PmicDxe -- DOES NOT EXIST ANYWHERE. The PMIC is a different chip from
  #   MT6358 and Mu-Silicium has zero MT6359 hits (UEFI-PLAN.md:142).
  # MsdcDxe / EmmcDxe / SdDxe / Fat -- MT6877 storage is UFS only; MsdcDxe would
  #   find no host (UEFI-PLAN.md:227-241).
  # A MediaTek UFS driver -- DOES NOT EXIST. New driver, not a copy.
  # A MediaTek USB device-mode driver -- DOES NOT EXIST, and HakiOS's loader
  #   binds a Qualcomm GUID (UEFI-PLAN.md:163-225). This is the blocker, stated
  #   in PLAN.md section 4.
  #
