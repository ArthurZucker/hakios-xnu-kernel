# `b651/uefi` — a scaffold for the MT6877 UEFI platform package

**Nothing in this directory builds.** There is no working firmware here, no edk2 tree, no
`build_uefi.py`, and nothing has been compiled or run. This is the *shape* of an
`mt6877Pkg` — the package Mu-Silicium would need before the HiBreak Pro could run HakiOS
through Project Silicium — written down so the port has a starting point and an honest
list of what is missing.

The research that this scaffold condenses is in [`../UEFI-PLAN.md`](../UEFI-PLAN.md)
(full derivation, path:line citations) and [`../HARDWARE.md`](../HARDWARE.md) (MT6877
addresses with sources). Read those before touching anything here.

## What this directory is

```text
uefi/
└── mt6877Pkg/                 <- named as if it sat at Silicon/MediaTek/MT6877Pkg/ in Mu-Silicium
    ├── mt6877.dsc             platform DSC: package graph, libraries, components, PCDs
    ├── mt6877.dec             package DEC: header paths, GUIDs, PCD declarations
    ├── Include/
    │   ├── APRIORI.inc        dispatch order of the DXE FV
    │   └── DXE.inc            DXE components, unordered
    └── Library/
        └── MemoryMapLib/
            ├── MemoryMapLib.c the one file with real content: the MT6877 memory map
            └── MemoryMapLib.inf
```

The paths inside these files assume the two-package split every MediaTek Mu-Silicium port
uses (`UEFI-PLAN.md` §2):

```text
Silicon/MediaTek/MT6877Pkg/     SoC facts       <- what this directory describes
Platforms/Bigme/b651Pkg/        board facts     <- NOT in this directory, not written yet
Silicon/MediaTek/MediaTekPkg/   SoC family, reused as-is
Silicon/Silicium/SiliciumPkg/   project-wide, reused as-is
```

Only the SoC half exists here. The board half (`b651Pkg`: `b651.dsc`, `b651.fdf`, keypad,
`DeviceBuild.py`, `Resources/Configs/b651.toml`) is the other 12 of the 26 files and is
**not** scaffolded — `MemoryMapLib` below cites what that board half must satisfy.

## Status

| Piece | State |
|---|---|
| `MemoryMapLib.c` | ⚠ real content: the map that Mu-Silicium's consumers demand, with MT6877 addresses. Every address is cited or marked `TODO`. Not compiled. |
| `mt6877.dsc` | ⚠ follows edk2 DSC format; **not validated by an edk2 parser** (no edk2 tree was available). HakiOS-only components are commented out with TODOs. |
| `mt6877.dec` | ⚠ same. It declares the two PCDs `MemoryMapLib.c` actually uses and nothing else. |
| `APRIORI.inc` / `DXE.inc` | ⚠ trimmed from the MT6789 `stonePkg` list to a milestone-1 set; every dropped driver is listed commented out with the reason. |
| Everything else | ✗ absent — see `PLAN.md` for the ordered list |

The **only** check that was run: `clang -fsyntax-only` on `MemoryMapLib.c` against
hand-written stubs of the three edk2 headers it includes. It parses. That is not a build,
and it says nothing about whether the edk2 tree accepts the `.dsc`, `.dec`, `.inf` or
`.inc` files.

There is deliberately **no** `mt6877Pkg.dsc.inc` (the SoC-class include Mu-Silicium's own
`MediaTekPkg.dsc.inc` pattern wants), no `PlatformSecLib`, no `ClockImplLib`, no
`MT6359PmicDxe`, no UFS driver and no USB-device driver. Each is in `PLAN.md` with the
reason it is not a copy.

## The first milestone

**One line of text out of the phone, on UART0 at `0x11002000`, 921600 8N1.**

Not on the screen: the panel is e-ink, no EPDC driver exists anywhere in HakiOS, in
Mu-Silicium or in XNU, and Mu-Silicium's `SimpleFbDxe` only registers a GOP over a buffer
something else already filled (`../SCOPE.md` gate 3, `UEFI-PLAN.md` §3.3). The only
output that can be relied on at milestone 1 is a serial port.

```text
BootShim -> Sec -> "Project Silicium ..." -> UART0 @0x11002000 -> a USB-serial adapter
```

Accepting criteria, in order — each is a separate experiment (`UEFI-PLAN.md` §4):

1. The FD builds at all (`python3 build_uefi.py -d b651 -r DEBUG`). Proves the package
   graph, not the phone.
2. Those `Sec.c` lines appear on a UART adapter. Proves `FD_BASE`, the watchdog key, the
   **UART pinmux** and where the bootloader put DDR.
3. The same happens when flash-booted from a boot partition (gate 1 passed first).
4. Only then is `Display Reserved` worth making real, which needs gate 3 (e-ink) first.

### What is honestly unknown about this milestone

- **UART0's pinmux.** If LK does not leave UART0 muxed and clocked, the firmware prints
  nothing. `../SCOPE.md:66` lists it as an open question; the answer is in the phone's
  own device tree, which needs root or BROM (`../PHONE.md`).
- **Where DDR is.** The only map available is a 998 MiB placeholder in an *OPPO* board's
  vendor DT (`../HARDWARE.md:52-53`). The HiBreak Pro's real map has not been read.
- **Whether an unsigned image runs at all.** Gate 1: `sys.oem_unlock_allowed=0` on this
  unit (`../PHONE.md`).

## What this scaffold is not

- Not a Mu-Silicium patch. It is a separately written skeleton; adapting it means copying
  it into a Mu-Silicium checkout at `Silicon/MediaTek/MT6877Pkg/` and fixing it against
  the real tree. **No Mu-Silicium tree was cloned or modified to make it.**
- Not accurate about edk2 syntax by construction: the `.dsc`/`.dec`/`.inc` files were
  written from the format Mu-Silicium uses, without a parser to check them. Expect to fix
  things that look right and are not.
- Not a driver port. The two large artefacts — `ClockImplLib.c` (~4000 lines of clock
  table) and a MediaTek UFS driver — are named in `PLAN.md` and otherwise untouched.
