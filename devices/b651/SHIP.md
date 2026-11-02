# Shipping the HiBreak Pro: what "flashable" means here

Short answer: **there is no flashable image for this phone today, and this
directory cannot produce one without reading the phone first.** What exists is
(a) a boot image packer that runs and round-trips, (b) a restore procedure that
prints itself instead of running, and (c) this file, which says exactly which
link in the chain is missing.

Nothing here has been run against a phone. No partition has been read.

---

## 1. The chain that has to work

```text
                        exists here?   state
preloader -> LK -> reads boot_a          no        LK is Bigme's, not ours
   |
   +-- LK verifies boot_a's signature    no        AVB 1.2, verified boot "green" (PHONE.md)
   |
   +-- LK copies 3 sections to the       no        addresses are in boot_a's header, unread
   |   header's load addresses, jumps
   |
   +-- arm64 kernel Image                PARTLY    hakios-kernel is Mach-O, not a Linux Image
   |                                                  (verified: no ARM\x64 at 0x38)
   |
   +-- kernel = HakiOS loader?           no        loader.efi is a UEFI app; a Linux
                                                      kernel cannot start it
   |
   +-- loader reads the XNUP payload     yes       qemu/payload.bin parses (4 entries)
   |
   +-- loader starts XNU                 unverified  never run on hardware
```

The two links that are not merely "not done" but **wrong-shaped** for this phone:

| Link | Says | But |
|---|---|---|
| kernel section | a boot image's kernel section holds an arm64 Linux `Image` (or gzip of one), entered at `kernel_addr` | `hakios-kernel` is a Mach-O 64 (`cf fa ed fe` at offset 0). It is not a Linux kernel and LK cannot enter it |
| kernel → loader | on a Qualcomm phone the chain is LK → U-Boot/LK's `boot` → UEFI app → XNU | On MT6877 the firmware that would run a UEFI app does not exist yet. `mt6877Pkg` is a scaffold (`uefi/PLAN.md`: nothing attempted, no build run) |

So even with perfect header fields, a boot image built today would be a
well-formed container around an artifact LK cannot execute.

---

## 2. The MediaTek boot image format

The container is a header followed by page-aligned sections. Field layout,
per-version trailer sizes and the full table are in the docstring of
[`tools/mkbootimg.py`](tools/mkbootimg.py) (`--help` prints it). The part that
matters for reading a stock image:

```text
offset  size  field
     0     8  "ANDROID!"
     8    32  kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
              second_size, second_addr, tags_addr, page_size   (8 x u32)
    40     8  AMBIGUOUS - see below
    48    16  name
    64   512  cmdline
   576    32  id (SHA-1 of sections + sizes, zero-padded)
   608  1024  extra_cmdline
```

### The two dialects at offset 40

| image age | 40 | 44 |
|---|---|---|
| pre-2018 (old AOSP and the MediaTek forks) | `dt_size` | `unused` (0) |
| 2018+ (header v0..v2) | `header_version` | `os_version` |

Nothing in the file says which. A reader that trusts `header_version` on an old
image sees `dt_size`, which is not 0..2. `mkbootimg.py --info` treats any value
above 2 as the old dialect and prints it as `dt_size`, so a stock dump can be
classified with one command.

### What is verified about the format, and how

| claim | how it was checked |
|---|---|
| v0 = 1632 B, v1 = 1648 B, v2 = 1660 B header | AOSP `bootimg.h` (`boot_img_hdr_v0` .. `_v4`) and `mkbootimg.py` constants `BOOT_IMAGE_HEADER_V{1,2}_SIZE`, read from android.googlesource.com |
| section `id` = SHA-1 over each section then its u32 size; empty sections contribute a 4-byte zero; v2 hashes the dtb | AOSP `mkbootimg.py` `update_sha()`, read from source |
| every section starts on a page boundary; `page_size` is 2048/4096 on these boards | AOSP header comments; the packer round-trips at 2048 |
| a 512-byte tagged sub-header may precede kernel and ramdisk (magic `88 16 88 58`) | community tool `Archive385/mkbootimg` `mkbootimg.c` / `unpackbootimg.c`. **Not** a MediaTek document, **not** checked against a real image |

The legacy 512-byte tag is the single least-trusted thing in the packer. It is
implemented and round-trips, and it is off unless `--mtk-header` is passed.

---

## 3. What the loader produces today

`xnuloader/mkpayload.py` writes an **XNUP** container, not a boot image:

```text
XNUP | u32 size
     | entries: name[16], size u32, reserved u32, data padded to 16
     | end marker (empty name)
```

`qemu/payload.bin` (built 2026-11-03 07:36, 15,954,608 bytes) parses as:

| entry | bytes | content |
|---|---|---|
| `kernel` | 15,937,800 | Mach-O 64 |
| `ramdisk` | 16,568 | Mach-O 64 |
| `cmdline` | 41 | `-v cpus=8 bootcpu=7 fbdev=1 virtio_gpu=1` |
| `kernel.ver` | 41 | `2.1.13-hakios-darwin22.5-g7f274bc5-dirty` |

The loader itself (`xnuloader/loader.c:18`) looks for that magic at a **fixed
physical address**:

```c
#define PAYLOAD_BASE    0xF0000000ULL
#define PAYLOAD_MAGIC   0x50554E58u /* "XNUP" */
```

**That address is written for a Qualcomm phone.** On those, a boot shim (built
by `mkxnuimg.py`, `PAYLOAD_BASE 0xF0000000` too) copies the payload there before
starting UEFI. For b651 there is no shim: `devices/redwood/` and the Mu-Silicium
tree that `mkxnuimg.py` and `mkubootimg.py` depend on are not in this checkout,
and an MT6877 UEFI does not exist. So `0xF0000000` is a claim about this phone's
memory map that nothing has checked.

`qemu/payload.bin` is produced by `qemu/run-b651.sh`, which loads it through
QEMU's loader device at `0xF0000000`. It is a simulator artifact.

---

## 4. The gap, stated as fields

| header field | packer default | what the phone needs | source |
|---|---|---|---|
| `magic` | `ANDROID!` | same | certain |
| `header_version` | 0 | unknown: 0–2 all plausible for Android 14 | the phone's `boot_a` |
| `page_size` | 2048 | unknown: 2048 or 4096 | `boot_a` |
| `kernel_addr` | `0x40080000` | unknown | `boot_a` |
| `ramdisk_addr` | `0x41000000` | unknown | `boot_a` |
| `tags_addr` | `0x40000100` | unknown | `boot_a` |
| `dtb_addr` | `0x41F00000` | unknown; the phone's dtb is a separate `dtbo_a` and possibly `vendor_boot` | `boot_a`, `dtbo_a` |
| `cmdline` | empty | unknown; `/proc/cmdline` is denied to `shell` (PHONE.md) | BROM or root |
| `kernel` section | any file | **an arm64 Linux `Image`** — which does not exist for this port | not produced anywhere |
| `ramdisk` section | any file | on Android 13+, `init_boot_a` holds the generic ramdisk; `boot_a` may expect one, none, or a vendor ramdisk | `boot_a` size and contents |
| signature | none | AVB 1.2 is in force (`ro.boot.vbmeta.avb_version=1.2`, `verifiedbootstate=green`) | `boot_a` + `vbmeta` |

The signature row deserves its own sentence: **a boot image without a valid AVB
signature will not boot on a locked device, whatever its header says.** An
unlocked `seccfg` changes that. `PHONE.md:104-116` reads the current unit as
locked with `sys.oem_unlock_allowed=0`, which means the toggle that would permit
one is greyed out. That is the gate, and it is above the format question.

Also unnamed by any doc here: whether this phone keeps a generic ramdisk in
`init_boot_a` (Android 13+ layout, and the phone lists that partition) means a
HakiOS `boot_a` image may be the wrong partition to replace at all. The
partition table read in GATE1 step 3 answers that; it has not been done.

---

## 5. What is unknown without reading `boot_a`

Everything in this list is unknown *only* because the phone has not been read.
Each one is one `mtk r boot_a` away, and this file deliberately does not guess
any of them.

```text
field / question                       answered by
-------------------------------------  ---------------------------------------
header dialect and version             boot_a bytes 40..48
page_size, all four load addresses     boot_a header
the real cmdline                       boot_a cmdline field + /proc/cmdline over BROM
whether boot_a holds a ramdisk at all  boot_a ramdisk_size
partition sizes (boot_a, init_boot_a)  partition table (GATE1 step 3)
AVB: is boot_a signed, with what key   boot_a tail + vbmeta partition
what LK actually checks before boot    lk_a (disassembly; not attempted)
free RAM for the payload at 0xF0000000 lk_a / preloader cmdline memory map
```

The restore side needs none of this — it restores bytes that were read, which
is the one part of shipping that is actually complete.

---

## 6. What was verified, with the exact commands

All run on macOS, from the repository root, with no phone attached.

### `mkbootimg.py --help`

```console
$ python3 devices/b651/tools/mkbootimg.py --help
usage: mkbootimg.py [-h] [--kernel KERNEL] [--ramdisk RAMDISK] [--no-ramdisk]
                    [--second SECOND] [--devicetree DTB] [--cmdline CMDLINE]
                    [--board BOARD] [-o OUTPUT] [--header-version {0,1,2}]
                    [--page-size {2048,4096,8192,16384}] [--base BASE]
                    [--kernel-offset KERNEL_OFFSET]
                    [--ramdisk-offset RAMDISK_OFFSET]
                    [--second-offset SECOND_OFFSET]
                    [--tags-offset TAGS_OFFSET]
                    [--devicetree-load-addr DTB_ADDR] [--mtk-header]
                    [--info IMG] [--unpack IMG]

Pack or unpack an Android/MediaTek boot image (stdlib only).
...
```

Exit status 0. The full body is the header field table.

### `restore.sh` syntax

```console
$ bash -n devices/b651/tools/restore.sh && echo "syntax OK"
syntax OK
```

### A real pack / inspect / unpack round-trip

Inputs: `kernel` 123,392 B, `ramdisk` 20,011 B, `dtb` 9,001 B (patterns, so any
misplacement is visible).

```console
$ python3 devices/b651/tools/mkbootimg.py --kernel /tmp/b651-verify/kernel \
    --ramdisk /tmp/b651-verify/ramdisk -o /tmp/b651-verify/boot-v0.img \
    --cmdline "console=ttyS0,921600n1"
/tmp/b651-verify/boot-v0.img: 147456 bytes, header v0, page 2048
  kernel       123392 bytes at 0x40080000 (61 pages)
  ramdisk       20011 bytes at 0x41000000 (10 pages)
  id       5d80e8209f4d403333b688aecc4010b4160c8d3f

$ python3 devices/b651/tools/mkbootimg.py --kernel /tmp/b651-verify/kernel \
    --ramdisk /tmp/b651-verify/ramdisk --devicetree /tmp/b651-verify/dtb \
    --header-version 2 --cmdline "console=ttyS0,921600n1" \
    -o /tmp/b651-verify/boot-v2.img
/tmp/b651-verify/boot-v2.img: 157696 bytes, header v2, page 2048
  kernel       123392 bytes at 0x40080000 (61 pages)
  ramdisk       20011 bytes at 0x41000000 (10 pages)
  dtb            9001 bytes at 0x41f00000 (5 pages)
  id       cfde85bd581fd9e075233274896e1edd8b8c8f69

$ python3 devices/b651/tools/mkbootimg.py --info /tmp/b651-verify/boot-v2.img
image        157696 bytes
magic        at offset 0
page_size    2048
header       version 2 (header_size field 1660)
os_version   0x00000000
name         ''
cmdline      'console=ttyS0,921600n1'

section                size   load address   pages  magic
kernel       123392 B (0.12 MiB)     0x40080000      61  00010203
ramdisk      20011 B (0.02 MiB)     0x41000000      10  52525252
dtb          9001 B (0.01 MiB)     0x41f00000       5  44444444

tags_addr    0x40000100
second_addr  0x40f00000

$ python3 devices/b651/tools/mkbootimg.py --unpack /tmp/b651-verify/boot-v2.img \
    -o /tmp/b651-verify/out
/tmp/b651-verify/boot-v2.img -> /tmp/b651-verify/out/: kernel, ramdisk, dtb
  kernel         123392 bytes  sha256 4bc6c0de88d2d71c07770b718be4c75e9fb5a66a219e9b27b4b80f37503f1322
  ramdisk         20011 bytes  sha256 57a1888c1523e976bdc3af55884362a5bac0781b56ddde8e3dcf12a4e938f25c
  dtb              9001 bytes  sha256 baf0cfc445bb838d13c76cc02112a3fe6b6679081978424e43a0c26bd98a5292

$ cmp /tmp/b651-verify/out/kernel /tmp/b651-verify/kernel && \
  cmp /tmp/b651-verify/out/ramdisk /tmp/b651-verify/ramdisk && \
  cmp /tmp/b651-verify/out/dtb /tmp/b651-verify/dtb && echo identical
identical
```

Beyond byte equality, a separate checker re-derived from the raw bytes:

| check | v0 | v1 | v2 | legacy MTK tag |
|---|---|---|---|---|
| `magic` at 0 | ✓ | ✓ | ✓ | ✓ |
| header version field | ✓ | ✓ | ✓ | ✓ (tag bytes) |
| section sizes/sections equal inputs | ✓ | ✓ | ✓ | ✓ (data after the 512-byte tag) |
| load addresses as intended | ✓ | ✓ | ✓ | — |
| `id` equals AOSP `update_sha()` recomputed independently | ✓ | ✓ | ✓ | — |
| `header_size` field = 1648 / 1660 | — | ✓ | ✓ | — |
| total size = header page + 61 + 10 (+5) pages | 147456 | 147456 | 157696 | 149504 |

v2 `id` byte-for-byte: `cfde85bd581fd9e075233274896e1edd8b8c8f69` + 12 zero bytes.

Header parsing also rejects a non-image cleanly:

```console
$ python3 devices/b651/tools/mkbootimg.py --info /tmp/b651-verify/kernel
mkbootimg.py: no "ANDROID!" magic in the first 512 bytes: this is not a boot image
(exit 1)
```

### `restore.sh` against a synthetic backup

`backup`, `verify`, `restore` were each run against a temporary directory of
random files with a `SHA256SUMS` taken at "dump time":

| case | result |
|---|---|
| all present and matching, `verify --run` | `7 verified, 0 failed`, exit 0 |
| one file altered after hashing | `boot_a MISMATCH ... 1 failed`, exit 2 |
| file missing that the slot does not need | reported `MISSING`, not a failure |
| `restore` after a failed verify | prints `Refusing to plan a restore from a backup that did not verify.` |
| `restore --slot b` where `boot_b` verified | prints the whole plan, exit 0 |
| default (no `--run`) | hashes are not computed; says so and exits 2 |
| `--slot c`, missing `--backup` | usage error, exit 1 |

Audit that nothing in it can reach a device: every `mtk` / `fastboot` / `adb`
line goes through `device_cmd()`, which only `say`s it. Grepping for those
binaries outside comments and `device_cmd` returns nothing.

---

## 7. What cannot be verified offline, said plainly

| thing | why it cannot be checked here |
|---|---|
| that any produced image boots | no LK, no bootloader emulator, no phone writes |
| that LK accepts the header at all | same; MT6877 LK's parser is unread |
| the load addresses / page size / header version | in `boot_a`, unread |
| the legacy 512-byte MediaTek tag | no MT6877/MK image to compare against; only a community tool |
| `0xF0000000` being free for the payload | never checked on this memory map |
| AVB / signature acceptance | needs `boot_a` and `vbmeta`; the device reads locked |
| whether `boot_a` or `init_boot_a` is the right partition | partition sizes unread |
| the loader starting XNU on this phone | never run on hardware; only in QEMU |

What *is* verified: the packer produces correctly structured v0/v1/v2 images and
reads them back byte-identically, its `id` matches AOSP's rule, and the restore
script is syntactically valid, correct against a synthetic backup, and contains
no path that writes to a phone.

---

## 8. The order that would actually ship this

Each step produces evidence the next one needs; none of them is a build.

| # | Step | Produces | Needs phone |
|---|---|---|---|
| 1 | `mtk r boot_a boot_a.img`, then `mkbootimg.py --info boot_a.img` | the header dialect, page size, every address, the cmdline, whether a ramdisk is present | yes |
| 2 | `mtk r seccfg` and a lock-state read (GATE1 step 6) | whether an unsigned image can run at all | yes |
| 3 | `restore.sh verify --run` on the dumps | a restore path that has been checked against hashes taken at dump time | no |
| 4 | a Linux arm64 `Image` that can start the loader (or a UEFI that can) | the missing link in §1 | no |
| 5 | `mkbootimg.py` with step 1's addresses, then `fastboot boot` (RAM only) | whether LK runs it | yes |

Step 1 is the cheapest and unblocks the most. Until it happens, **this directory
can produce a well-formed boot image, not a bootable one**, and it says so
because the alternative is a file that looks right and bricks a phone.
