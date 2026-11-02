/** @file
  The MT6877 memory map, as the Project Silicium (Mu-Silicium) stack needs it.

  SCAFFOLD ONLY. This file has real content but has never been built: no
  Mu-Silicium tree and no edk2 tree were present when it was written. The only
  check performed was a standalone `clang -fsyntax-only` against hand-written
  stubs of <Uefi.h>, BaseLib's AsciiStrCmp and DebugLib's ASSERT -- it parses,
  which proves nothing about edk2. The descriptor layout below mirrors what
  UEFI-PLAN.md:51 reports of
  Platforms/Xiaomi/stonePkg/Library/MemoryMapLib/MemoryMapLib.c:

      {Name, Address, Length, HobOption, ResourceType, ResourceAttribute,
       MemoryType, ArmAttribute}

  The accessor names and signature are this scaffold's own invention and MUST be
  reconciled with the real MemoryMapLib before anyone tries to build it. What is
  load-bearing here is the CONTENTS of the tables, not the C around them.

  Provenance convention
  ---------------------
  Every address carries `HARDWARE.md` + the line it came from, or `TREE` + the line
  in devices/b651/dumps/devicetree-nodes.txt (the phone's own device-tree node
  names, which carry their unit addresses), or TODO with the question that answers
  it. No address below was invented.

  HARDWARE.md's own line numbers point into Winkmoon/android_kernel_xinran_oppo_mt6877
  (an OPPO Reno 7 SE, same silicon). Node names in dumps/devicetree-nodes.txt are
  read off the HiBreak Pro itself, so where the two disagree the dump is stronger —
  but the dump has no property VALUES, only node names.

  Nothing here is board-specific to the HiBreak Pro. The board map
  (Platforms/Bigme/b651Pkg/Library/MemoryMapLib/MemoryMapLib.c in a real tree) is
  the one that should own the DRAM window and the display buffer; this file
  declares the SoC's register windows and the region NAMES every driver looks up.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>          // AsciiStrCmp
#include <Library/DebugLib.h>         // ASSERT

//
// TODO: replace with the real header. MT6789Pkg does not appear to ship one
// (UEFI-PLAN.md 1a lists no Include/ under it), so the declaration probably lives
// in MediaTekPkg or a Silicium-wide header. The struct below is written from the
// field list in UEFI-PLAN.md:51 and the field TYPES are guesses.
//
typedef struct {
  CONST CHAR8 *Name;
  UINT64       Address;
  UINT64       Length;
  BOOLEAN      HobOption;        // TRUE: become a HOB memory descriptor
  UINT32       ResourceType;     // EFI_RESOURCE_SYSTEM_MEMORY / _MEMORY_MAPPED_IO
  UINT32       ResourceAttribute;
  UINT32       MemoryType;       // EfiConventionalMemory / EfiReservedMemoryType / ...
  UINT32       ArmAttributes;    // ARM_MEMORY_REGION_ATTRIBUTE_*
} MT6877_MEMORY_REGION;

#define MT6877_ARRAY_COUNT(a)  (sizeof (a) / sizeof ((a)[0]))

//
// ---------------------------------------------------------------------------
// 1. RAM regions.
//
//    The regions Mu-Silicium's consumers demand, by name (UEFI-PLAN.md:1c).
//    A region that is missing is not a warning at runtime: Sec.c takes
//    "DXE Heap" as the HOB list AND the whole UEFI heap (Sec.c:62-63,71-78),
//    and MemoryInitPeiLib CpuDeadLoops on an overlap or a missing heap
//    (MemoryInitPei.c:65,79-81,123).
//
//    Milestone 1 needs the first five. The rest are the real map and are mostly
//    TODO because their offsets are chosen by the bootloader at runtime.
// ---------------------------------------------------------------------------
STATIC CONST MT6877_MEMORY_REGION mMemoryMap[] = {
  //
  // The UEFI FD. Sec runs out of this, so the address and size must be exactly
  // where LK puts it.
  //
  // TODO (unsourced): Base and size. LK decides both; Project Silicium's
  //       build_uefi.py takes them from Resources/Configs/b651.toml
  //       ([uefi_fd] base/size, UEFI-PLAN.md:143) and this array must agree with
  //       that file. 2 MiB is a guess from UEFI-PLAN.md:310, not a measurement.
  {
    "UEFI FD",
    0x0000000000000000ULL,       // TODO: from b651.toml [uefi_fd] base
    0x0000000000200000ULL,       // TODO: 2 MiB, chosen by hand (UEFI-PLAN.md:310)
    TRUE,
    0 /* EFI_RESOURCE_MEMORY_RESERVED */,
    0,
    4 /* EfiRuntimeServicesData */,
    0
  },

  //
  // SEC/PEI stack. 256 KiB chosen by hand (UEFI-PLAN.md:310).
  //
  // TODO (unsourced): base. Any address inside the DRAM window below the first
  //       reserved block works; it must not collide with the FD.
  {
    "UEFI Stack",
    0x0000000000000000ULL,       // TODO: base, board choice
    0x0000000000040000ULL,       // 256 KiB (UEFI-PLAN.md:310)
    TRUE, 0, 0, 4 /* EfiRuntimeServicesData */, 0
  },

  //
  // The DXE heap. REQUIRED (Sec.c:62-63). 60 MiB chosen by hand.
  //
  // TODO (unsourced): base. Constraint that IS known: it must sit below the
  //       lowest runtime-chosen reserved block and must not overlap the FD or the
  //       stack. On the OPPO board the first reserved block found is mcupm_share,
  //       whose alloc-range starts at DRAM base (HARDWARE.md:55), so "below the
  //       first reserved region" is not yet answerable.
  {
    "DXE Heap",
    0x0000000000000000ULL,       // TODO: base, board choice
    0x0000000003C00000ULL,       // 60 MiB (UEFI-PLAN.md:310)
    TRUE, 1 /* EFI_RESOURCE_SYSTEM_MEMORY */, 0,
    7 /* EfiConventionalMemory */,
    0x6 /* ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK */
  },

  //
  // Display buffer behind SimpleFbDxe's GOP (SimpleFb.c:96-104) and, in DEBUG
  // builds, behind FrameBufferSerialPortLib -- the only "serial port" the MTK
  // boards have (UEFI-PLAN.md:71).
  //
  // Milestone 1 FAKES this into the DXE heap (UEFI-PLAN.md:310) so those two do
  // not abort. That is a lie the build can carry: a GOP over 3 MB of heap is not
  // a panel, and on this phone there is no panel driver at all (e-ink, SCOPE.md
  // gate 3). The real buffer needs the EPDC base, which is unknown.
  //
  // TODO (unsourced): the EPD frame buffer base. HARDWARE.md:64 records the OPPO
  //       board's `atag,videolfb-fb_base_l = <0x7e605000>` -- a different board's
  //       bootloader hint, not this phone's. The phone's /eink node has the
  //       geometry but its property values need root (PHONE.md) and
  //       dumps/devicetree-nodes.txt carries names only.
  {
    "Display Reserved",
    0x0000000000000000ULL,       // TODO: inside DXE Heap for milestone 1
    0x0000000000300000ULL,       // TODO: 3 MiB placeholder, not W*H*Bpp
    FALSE,
    1 /* EFI_RESOURCE_SYSTEM_MEMORY */, 0,
    7 /* EfiConventionalMemory */, 0
  },

  //
  // --- The real map below this line is NOT for milestone 1. ---
  //

  //
  // The DRAM window. Base is solid; the extent is a placeholder.
  //
  // HARDWARE.md:52: the vendor DT's memory node is
  // `<0 0x40000000 0 0x3e605000>` = 998 MiB, top 0x7e605000.
  // HARDWARE.md:53: that node is a fixed stub; the fitted size comes from LK and
  //   was never read. PHONE.md calls the unit "8 GB class" -- a marketing figure,
  //   not a memory-map fact.
  //
  // TODO: the real size. Two ways to get it: (a) read it out of the EMI
  //       controller at runtime, which is what RamManagerLib already does
  //       (RamManager.c:125 looks up a region named "EMI"), or (b) `adb shell
  //       cat /proc/meminfo` / the phone's own DT, both blocked without root
  //       (PHONE.md).
  //
  // NOTE the internal inconsistency that proves the stub is a stub: SSMR CMA is
  // allocated at 0xc0000000 (HARDWARE.md:54), which is ABOVE 0x7e605000. A
  // 998 MiB map and that carveout cannot both be true of this SoC.
  {
    "System RAM",
    0x0000000040000000ULL,       // HARDWARE.md:52  (mt6877.dts:585-588)
    0x000000003E605000ULL,       // HARDWARE.md:52  STUB size, not the fitted one
    FALSE,
    1 /* EFI_RESOURCE_SYSTEM_MEMORY */, 0,
    7 /* EfiConventionalMemory */,
    0x6 /* WRITE_BACK */
  },

  //
  // SSMR CMA: 256 MiB, reusable.
  // HARDWARE.md:54 (mt6877.dts:600-606): alloc-range <0 0xc0000000 0 0x10000000>.
  //
  // This is a SoC-wide range that the modem/media stack draws from. Handing it to
  // XNU would be handing XNU memory the modem is using (UEFI-PLAN.md:136).
  {
    "SSMR CMA",
    0x00000000C0000000ULL,       // HARDWARE.md:54
    0x0000000010000000ULL,       // HARDWARE.md:54  256 MiB
    FALSE, 1, 0, 5 /* EfiReservedMemoryType */, 0
  },

  //
  // MCUPM share: 6 MiB + 64 KiB = 0x610000, no-map, alloc-range
  // 0x40000000..0xa0000000 (HARDWARE.md:55).
  //
  // TODO (unsourced): the offset WITHIN that range. The alloc-range is a
  //       constraint, not an address; LK picks the actual base. Every
  //       reserved-memory block on the OPPO DT is like this, and the phone's own
  //       mblock-* names (PHONE.md:77) are visible without root but their values
  //       are not.
  {
    "MCUPM Share",
    0x0000000040000000ULL,       // TODO: inside alloc-range, base unknown
    0x0000000000610000ULL,       // HARDWARE.md:55  6 MiB + 64 KiB
    FALSE, 0, 0, 5 /* EfiReservedMemoryType */, 0
  },

  //
  // Remaining carveouts: SIZES ARE KNOWN, ADDRESSES ARE NOT.
  // HARDWARE.md:56 (mt6877.dts:632-686) gives only sizes:
  //   consys 0x780000, scp_share 0x1a0000, sspm_share 0x510000,
  //   adsp_share 0x1000000, wifi 0xf20000, gps 0x100000, ion carveout 0xc000.
  //
  // TODO: every base. Until they are known, reserving them by size alone is
  //       impossible, and NOT reserving them means the firmware may hand XNU
  //       memory a DSP or the connectivity stack owns. This is the single largest
  //       correctness hole in this file. It is a READ-before-boot item, not a
  //       build item.
  //
  // One entry is written out as the template for the other six, so the shape is
  // visible rather than only described:
  {
    "SSPM Share",
    0x0000000040000000ULL,       // TODO: base, unknown
    0x0000000000510000ULL,       // HARDWARE.md:56  size only
    FALSE, 0, 0, 5 /* EfiReservedMemoryType */, 0
  },
  // TODO: consys (0x780000), scp_share (0x1a0000), adsp_share (0x1000000),
  //       wifi (0xf20000), gps (0x100000), ion carveout (0xc000). Same shape,
  //       bases unknown (HARDWARE.md:56).
  //
  // TODO: the e-ink carveouts. PHONE.md:77 lists mblock-20-waveform,
  //       mblock-21/22/24-eink0/1/2, mblock-23-vcom, mblock-25-eink_temp_buf,
  //       mblock-40-vcom as reserved by name on THIS phone. Not one of their
  //       addresses or sizes has been read. A firmware that does not reserve
  //       them will corrupt the panel's waveform LUTs -- which live on their own
  //       partitions (`waveform`, `vcom`) and are per-unit calibration.
};

//
// ---------------------------------------------------------------------------
// 2. Register windows (MMIO).
//
//    Mu-Silicium finds these by name. The names on the left of each comment are
//    the lookups in UEFI-PLAN.md:1c -- misspell one and the driver silently
//    finds nothing.
//
//    Only UART0 is used by milestone 1, and it is NOT reached through this table
//    (SerialPortLib takes PcdSerialRegisterBase). The rest are declared so the
//    map is complete and so the drivers that will need them have their address
//    written down in one place.
// ---------------------------------------------------------------------------
STATIC CONST MT6877_MEMORY_REGION mRegisterMap[] = {
  //
  // --- Milestone 1 ---
  //

  // "WatchDog Timer" -- PlatformSecLib writes WDT_MODE_KEY here at SEC time
  // (PlatformSec.c:19). Without it the watchdog reboots the phone mid-boot.
  // HARDWARE.md:62 (mt6877.dts:1459-1464): toprgu@10007000, GIC_SPI 109.
  {
    "WatchDog Timer",
    0x0000000010007000ULL,       // HARDWARE.md:62
    0x0000000000001000ULL,       // one 4 KiB page; exact syscon length unsourced
    FALSE, 2 /* EFI_RESOURCE_MEMORY_MAPPED_IO */, 0, 11 /* EfiMemoryMappedIO */,
    0x1 /* ARM_MEMORY_REGION_ATTRIBUTE_DEVICE */
  },

  // UART0 -- 16550 at stride 4. Not consumed through this table (SerialPortLib
  // takes PcdSerialRegisterBase = this address), but declared so the two agree.
  // HARDWARE.md:13 (mt6877.dts:1844-1846): serial@11002000, 0x1000, GIC_SPI 141.
  {
    "UART0",
    0x0000000011002000ULL,       // HARDWARE.md:13
    0x0000000000001000ULL,       // HARDWARE.md:13
    FALSE, 2, 0, 11, 0x1
  },

  // "Pmic Wrapper" -- PmicWrapperDxe (PmicWrapperDxe.c:137). Not needed at
  // milestone 1 but declared: it is the reason keys work at all.
  // HARDWARE.md:27 (mt6877.dts:1588-1593): pwrap@10026000 +0x1000,
  // mediatek,mt6877-pwrap, GIC_SPI 264.
  {
    "Pmic Wrapper",
    0x0000000010026000ULL,       // HARDWARE.md:27
    0x0000000000001000ULL,       // HARDWARE.md:27
    FALSE, 2, 0, 11, 0x1
  },

  //
  // --- Declared for later, not used by milestone 1 ---
  //

  // "Pinctrl" -- GpioDxe (GpioDxe.c:153).
  // TODO (unsourced): the address. HARDWARE.md does not give MT6877's pinctrl
  //       base, and the device-tree dump has a `pinctrl` node with no unit
  //       address (dumps/devicetree-nodes.txt:362). MTK's pinctrl is conventionally
  //       at 0x10005000, which is exactly the kind of "convention" this file
  //       refuses to invent.
  {
    "Pinctrl",
    0x0000000000000000ULL,       // TODO: pinctrl base
    0x0000000000001000ULL,       // TODO
    FALSE, 2, 0, 11, 0x1
  },

  // "AP Mixed" -- ClockDxe (ClockDxe.c:629).
  // HARDWARE.md:66 mentions apmixedsys @0x1000c000, but in a row marked "assumed"
  // and in passing (while flagging dvfsp@0x0011bc00 as implausible). Treat the
  // value as a lead, not a fact.
  //
  // TODO: confirm 0x1000c000 against the phone's DT.
  {
    "AP Mixed",
    0x000000001000C000ULL,       // HARDWARE.md:66 (assumed row)
    0x0000000000001000ULL,       // TODO
    FALSE, 2, 0, 11, 0x1
  },

  // "Top Ck Gen" and "Infra Cfg Ao" -- ClockDxe (ClockDxe.c:636,643).
  // TODO (unsourced): both addresses. Not in HARDWARE.md at all.
  {
    "Top Ck Gen",
    0x0000000000000000ULL,       // TODO
    0x0000000000001000ULL,       // TODO
    FALSE, 2, 0, 11, 0x1
  },
  {
    "Infra Cfg Ao",
    0x0000000000000000ULL,       // TODO
    0x0000000000001000ULL,       // TODO
    FALSE, 2, 0, 11, 0x1
  },

  // "EMI" -- RamManagerLib reads DRAM geometry out of the controller rather than
  // out of this map (RamManager.c:125).
  // TREE: dumps/devicetree-nodes.txt:161 `dramc@10230000`; the per-channel blocks
  // are at :129-160 (ch0 0x10230000.., ch1 0x10240000..). Which base the vendor
  // driver binds is not established; 0x10230000 is the ch0 top block.
  {
    "EMI",
    0x0000000010230000ULL,       // TREE:161
    0x0000000000001000ULL,       // TODO: real span (ch0+ch1 windows are ~16 blocks)
    FALSE, 2, 0, 11, 0x1
  },

  // GICv3. Both windows are needed before any interrupt exists.
  // HARDWARE.md:23 (mt6877.dts:700-711): dist 0x0c000000 +0x40000,
  // redist 0x0c040000 +0x200000, one redist region.
  {
    "GIC Distributor",
    0x000000000C000000ULL,       // HARDWARE.md:23
    0x0000000000040000ULL,       // HARDWARE.md:23
    FALSE, 2, 0, 11, 0x1
  },
  {
    "GIC Redistributor",
    0x000000000C040000ULL,       // HARDWARE.md:23
    0x0000000000200000ULL,       // HARDWARE.md:23
    FALSE, 2, 0, 11, 0x1
  },

  // UFS host + PHY. No driver exists (UEFI-PLAN.md 3.2); declared so the address
  // is written down somewhere.
  // HARDWARE.md:32 (mt6877.dts:2543-2546): ufshci@11270000, 0x2300, GIC_SPI 137.
  {
    "UFS Host",
    0x0000000011270000ULL,       // HARDWARE.md:32
    0x0000000000002300ULL,       // HARDWARE.md:32
    FALSE, 2, 0, 11, 0x1
  },
  // HARDWARE.md:34 (mt6877.dts:2583-2586): ufs_mphy@11fa0000 +0xc000.
  {
    "UFS MPHY",
    0x0000000011FA0000ULL,       // HARDWARE.md:34
    0x000000000000C000ULL,       // HARDWARE.md:34
    FALSE, 2, 0, 11, 0x1
  },

  // Thermal (LVTS): node is `therm_ctrl`, not `lvts`.
  // HARDWARE.md:36 (mt6877.dts:4964-4971): 0x1100b000, 0x26e200, GIC_SPI 213/214.
  {
    "Thermal",
    0x000000001100B000ULL,       // HARDWARE.md:36
    0x000000000026E200ULL,       // HARDWARE.md:36
    FALSE, 2, 0, 11, 0x1
  },

  // Display: dispsys + the two overlays. Nothing in Mu-Silicium programs these
  // (SimpleFbDxe only consumes "Display Reserved"), and the panel is e-ink; the
  // addresses are recorded for the day an EPDC driver exists.
  // HARDWARE.md:59 (mt6877.dts:3381-3385): dispsys_config@14000000.
  {
    "Dispsys Config",
    0x0000000014000000ULL,       // HARDWARE.md:59
    0x0000000000001000ULL,       // TODO: span
    FALSE, 2, 0, 11, 0x1
  },
  // HARDWARE.md:61 (mt6877.dts:3952,3964): ovl0 @14005000, ovl0_2l @14006000.
  {
    "OVL0",
    0x0000000014005000ULL,       // HARDWARE.md:61
    0x0000000000001000ULL,
    FALSE, 2, 0, 11, 0x1
  },
  // HARDWARE.md:60 (mt6877.dts:4086-4096): dsi0 @14013000, GIC_SPI 313.
  {
    "DSI0",
    0x0000000014013000ULL,       // HARDWARE.md:60
    0x0000000000001000ULL,
    FALSE, 2, 0, 11, 0x1
  },

  // PMIF: the SPMI master for the two MT6315 bucks (VPROC1/VPROC2). Separate
  // from pwrap, and NOT an SPMI arbiter of the shape HakiOS's kernel expects
  // (HARDWARE.md:91-98).
  // HARDWARE.md:28 (mt6877.dts:2750-2757): pmif 0x10027000+0x8ff,
  // pmifmpu 0x10027900+0x500, spmimst 0x10029000+0x100, GIC_SPI 265.
  {
    "PMIF",
    0x0000000010027000ULL,       // HARDWARE.md:28
    0x00000000000008FFULL,       // HARDWARE.md:28
    FALSE, 2, 0, 11, 0x1
  },

  // UART1: present on the SoC, `disabled` on the OPPO board (HARDWARE.md:22).
  // Whether the HiBreak Pro uses it is unknown; declared for completeness.
  {
    "UART1",
    0x0000000011003000ULL,       // HARDWARE.md:22
    0x0000000000001000ULL,       // TODO: length not stated for UART1
    FALSE, 2, 0, 11, 0x1
  },
};

//
// ---------------------------------------------------------------------------
// 3. Accessors.
//
//    SCAFFOLD-LOCAL API. See the file header: these names and the signature are
//    invented here and must be replaced by whatever the real MemoryMapLib and
//    MemoryInitPei expect. They are written this way only so the two tables have
//    an owner and the borrow-return convention is explicit.
// ---------------------------------------------------------------------------

/**
  Return the RAM regions (the ones MemoryInitPeiLib validates and HOBs).

  @param[out] Regions  Borrowed pointer to the static array. Do not free.
  @param[out] Count    Number of entries.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
Mt6877GetMemoryRegions (
  OUT CONST MT6877_MEMORY_REGION **Regions,
  OUT UINTN                       *Count
  )
{
  ASSERT (Regions != NULL);
  ASSERT (Count != NULL);

  *Regions = mMemoryMap;
  *Count   = MT6877_ARRAY_COUNT (mMemoryMap);

  return EFI_SUCCESS;
}

/**
  Return the MMIO register windows.

  @param[out] Regions  Borrowed pointer to the static array. Do not free.
  @param[out] Count    Number of entries.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
Mt6877GetRegisterRegions (
  OUT CONST MT6877_MEMORY_REGION **Regions,
  OUT UINTN                       *Count
  )
{
  ASSERT (Regions != NULL);
  ASSERT (Count != NULL);

  *Regions = mRegisterMap;
  *Count   = MT6877_ARRAY_COUNT (mRegisterMap);

  return EFI_SUCCESS;
}

/**
  Sanity-check the map before anything else trusts it.

  Deliberately minimal. It catches the mistakes a scaffold makes -- a duplicate
  name, a region of length zero, a region not 4 KiB aligned -- and NOT the ones
  that matter most, which are wrong-but-plausible addresses. It cannot catch an
  overlap between two regions with the same base 0x0 (every TODO above), because
  they all start at zero.

  @retval EFI_SUCCESS              No scaffold-level mistake found.
  @retval EFI_INVALID_PARAMETER    A name is missing, or a length is zero.
  @retval EFI_ALREADY_STARTED      Two regions share a name.
  @retval EFI_UNSUPPORTED          A region is not 4 KiB aligned.
**/
EFI_STATUS
EFIAPI
Mt6877ValidateMemoryMap (
  VOID
  )
{
  UINTN  Index;
  UINTN  Other;

  for (Index = 0; Index < MT6877_ARRAY_COUNT (mMemoryMap); Index++) {
    if ((mMemoryMap[Index].Name == NULL) || (mMemoryMap[Index].Length == 0)) {
      return EFI_INVALID_PARAMETER;
    }

    if ((mMemoryMap[Index].Address & 0xFFF) != 0) {
      return EFI_UNSUPPORTED;
    }

    for (Other = Index + 1; Other < MT6877_ARRAY_COUNT (mMemoryMap); Other++) {
      if (AsciiStrCmp (mMemoryMap[Index].Name, mMemoryMap[Other].Name) == 0) {
        return EFI_ALREADY_STARTED;
      }
    }
  }

  return EFI_SUCCESS;
}

//
// NOTE, stated because it is the honest status of this file: on the day the
// addresses above are filled in, Mt6877ValidateMemoryMap() will still pass while
// the map is wrong in the ways that matter -- reserved blocks whose bases are
// unknown are all written 0x40000000, so they overlap each other and "System RAM"
// and nothing here objects. MemoryInitPeiLib WOULD dead-loop on that. Do not point
// a real build at this file until HARDWARE.md's reserved-memory bases are read
// off the phone.
//
