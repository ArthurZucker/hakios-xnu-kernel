## @file
#  APRIORI DXE dispatch order for b651 (MediaTek MT6877).
#
#  SCAFFOLD ONLY -- never parsed, never built, and the INF paths below are
#  RECONSTRUCTED, not read from a tree. Mu-Silicium's stonePkg has this file as a
#  plain list of INF paths included into the FDF's APRIORI section
#  (UEFI-PLAN.md:49); the list here has the same shape and the same purpose, but
#  every path is a TODO until it is checked against the real
#  SiliciumPkg/MediaTekPkg trees.
#
#  Why a fixed order at all: DXE dispatch normally follows the dependency graph,
#  and APRIORI overrides it for the handful of drivers whose order is not
#  expressible as a dependency -- the GIC must exist before anything takes an
#  interrupt, the metronome before anything takes a timer, and the storage stack
#  before BDS looks for a boot option.
#
#  WHAT IS DIFFERENT FROM stonePkg
#  ------------------------------
#  The MTK SoC drivers are GONE from this list, not reordered. Milestone 1 has no
#  clock table, no GPIO pin count and no MT6359P driver, so dispatching them would
#  hang or fault before the first line of text. They are listed commented out at
#  the bottom, in the order they come back (PLAN.md section 3).
##

#
# ---------------------------------------------------------------------------
# Core: must be first, or nothing else has a dispatcher.
# ---------------------------------------------------------------------------
MdeModulePkg/Core/Dxe/DxeMain.inf
MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

#
# ---------------------------------------------------------------------------
# Runtime services + CPU + interrupt controller + timers.
#
# ArmGicDxe takes its two windows from PcdGicDistributorBase /
# PcdGicRedistributorsBase (0x0C000000 / 0x0C040000, HARDWARE.md:23), NOT from
# MemoryMapLib. Both values are in mt6877.dsc.
# ---------------------------------------------------------------------------
MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
ArmPkg/Drivers/CpuDxe/CpuDxe.inf
ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
ArmPkg/Drivers/TimerDxe/TimerDxe.inf
MdeModulePkg/Universal/Metronome/Metronome.inf

#
# ---------------------------------------------------------------------------
# THE FIRST OUTPUT. Everything above this line is silent.
#
# Sec.c already printed its two lines at SEC time (UEFI-PLAN.md:321-322) -- those
# went out through SerialPortLib before DXE existed at all, so this list is not
# what makes milestone 1 observable. This is the console for everything AFTER:
# BDS, the boot manager, and HakiOS's loader.
#
# Note these do NOT program UART0. UART output is SerialPortLib, bound in
# mt6877.dsc; ConSplitter/Terminal are the UEFI console on top of whichever
# serial device exists.
# ---------------------------------------------------------------------------
MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf

#
# GraphicsConsoleDxe needs a GOP, and the only thing that produces one is
# SimpleFbDxe over the "Display Reserved" region (UEFI-PLAN.md:70). It must come
# after the console splitter and before BdsDxe.
#
# TODO: the INF path is unverified. On this phone the GOP is pure memory -- no
#       panel driver exists -- so including it changes nothing visible; it is
#       here so the console stack is complete and so the region name is consumed.
#
# SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf

#
# ---------------------------------------------------------------------------
# Boot manager last. It is what reaches HakiOS's loader.
#
# In the HakiOS build, PlatformBootManagerUnableToBoot() is patched to call
# XnuLoaderMain() (HakiOS/hakios.patch:486). With no storage driver there is
# nothing to boot, so that path is the only one taken -- which on this scaffold is
# a feature, and stops being one as soon as UFS works (UEFI-PLAN.md:99-101).
# ---------------------------------------------------------------------------
MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

#
# ---------------------------------------------------------------------------
# NOT DISPATCHED AT MILESTONE 1
#
# Every entry below is from stone's list (UEFI-PLAN.md:49) and is left out for a
# stated reason. Uncomment in the order given once the step before it passes.
# ---------------------------------------------------------------------------

#
# 1. GPIO + clock. Unblocks everything else. Missing: ClockImplLib's ~4000-line
#    table (ported from clk-mt6877*.c), MT6877's pin count, and the "Pinctrl" /
#    "AP Mixed" / "Top Ck Gen" / "Infra Cfg Ao" addresses -- three of the five are
#    TODO in MemoryMapLib.c.
#
# MediaTekPkg/Drivers/GpioDxe/GpioDxe.inf
# MediaTekPkg/Drivers/ClockDxe/ClockDxe.inf
#
# 2. PMIC. pwrap first (0x10026000, HARDWARE.md:27), then the PMIC driver --
#    which DOES NOT EXIST: MediaTekPkg ships MT6358PmicDxe and MT6877's PMIC is
#    MT6359P, a different chip (UEFI-PLAN.md:142). Writing it is ~600 lines.
#
# MediaTekPkg/Drivers/PmicWrapperDxe/PmicWrapperDxe.inf
# MediaTekPkg/Drivers/MT6358PmicDxe/MT6358PmicDxe.inf   # WRONG CHIP, do not use
# b651Pkg/Drivers/MT6359PmicDxe/MT6359PmicDxe.inf       # does not exist yet
#
# 3. Storage. NO MediaTek driver exists: MsdcDxe is eMMC/SD, this SoC is UFS-only,
#    and there is no UfsDxe under Silicon/MediaTek (UEFI-PLAN.md:227-241).
#    Enabling MsdcDxe here would dispatch a driver with no host to find.
#
# MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.inf
# MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
# MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf
# MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
# MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
# FatPkg/EnhancedFatDxe/Fat.inf
#
# 4. Keypad. Needs the PMIC path above; the keys are on MT6359P through pwrap,
#    not PMIF (HARDWARE.md:31).
#
# b651Pkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
#
# 5. USB device mode. DOES NOT EXIST for MediaTek anywhere in Mu-Silicium, and
#    HakiOS's loader binds Qualcomm's GUID. This is the delivery blocker; see
#    PLAN.md section 4. Nothing to uncomment -- there is no file to point at.
