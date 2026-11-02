## @file
#  DXE components for b651 (MediaTek MT6877), unordered.
#
#  SCAFFOLD ONLY -- never parsed, never built. Same caveat as APRIORI.inc: the
#  paths are reconstructed from the stonePkg component list (UEFI-PLAN.md:50),
#  not read out of a tree. This file is the "what is in the FV" list; APRIORI.inc
#  is the "and in what order" list. They must name the same set.
#
#  Deliberately the SAME set as APRIORI.inc's uncommented entries, no more. A DXE
#  component here that APRIORI does not order is a component whose dispatch is
#  left to the dependency graph -- fine for most, not fine for the GIC.
##

MdeModulePkg/Core/Dxe/DxeMain.inf
MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
ArmPkg/Drivers/CpuDxe/CpuDxe.inf
ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
ArmPkg/Drivers/TimerDxe/TimerDxe.inf
MdeModulePkg/Universal/Metronome/Metronome.inf

#
# Device path + reset + variable are what the loader touches before it can start:
# reset_system on every exit path, and a device path on any handle it opens
# (UEFI-PLAN.md:1d).
#
MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
MdeModulePkg/Universal/ResetDxe/ResetDxe.inf
MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf

#
# SimpleFbDxe registers a GOP over the "Display Reserved" region and nothing else
# (UEFI-PLAN.md:70). It is included because HakiOS's loader draws its banner
# through GOP; on this phone there is no panel driver, so the GOP is memory.
#
# TODO: SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf -- path unverified.
# SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf

MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

#
# ---------------------------------------------------------------------------
# NOT IN THE FV AT MILESTONE 1 -- see APRIORI.inc for the same list with the
# reasons, and PLAN.md section 3 for the order they come back.
#
#   GpioDxe, ClockDxe                 -- no clock table, no pin count, 3 of 5
#                                        clock addresses TODO in the map
#   PmicWrapperDxe, MT6359PmicDxe     -- the MT6359 driver does not exist
#   MsdcDxe, EmmcDxe, SdDxe, Fat      -- UFS-only SoC, no UFS driver exists
#   KeypadDeviceLib / keypad DXE      -- needs the PMIC path
#   any USB driver                    -- does not exist for MediaTek
# ---------------------------------------------------------------------------
