# Shipping the HiBreak Pro: what "flashable" means here

Short answer: **there is no image this phone would boot, and no verified way
back if one were written.** What exists is (a) a boot image packer that runs,
round-trips byte-identically and can wrap the real kernel, (b) a restore
procedure that is correct against a *synthetic* backup and has never been
pointed at a real one, and (c) this file, which says exactly which link is
missing.

Two things are weaker than a shipping story needs, and both are below.
**No partition has ever been read from this phone, so no backup exists** — the
`dumps/` directory holds two text files copied out of `/proc`, not images.
**Every write path is blocked or unverified on this unit** (§5a): fastboot is
refused by the lock, and BROM has never been exercised.

---

## 0. What this directory produces today

Every row was built or measured on 2026-11-03 with no phone attached, and each
is a *local file*, not a flashable one.

| artifact | built by | size (bytes) | what it is |
|---|---|---|---|
| `hakios-kernel` | `make kernel` (`xnu-build.sh`) | 15,938,800 | **Mach-O 64 arm64** (`cf fa ed fe` at 0), not a Linux `Image` |
| `xnuloader/loader.efi` | `xnuloader/build.sh` | 66,048 | PE32+ EFI AArch64 app; rebuilt cleanly here |
| `qemu/payload.bin` | `xnuloader/mkpayload.py` | 15,955,552 | **XNUP** container (§3) — QEMU loader device only |
| `/tmp/…/boot-v0.img` | `mkbootimg.py` (synthetic inputs) | 147,456 | correct v0 container, round-trips |
| `/tmp/…/boot-real-v2.img` | `mkbootimg.py` on the real kernel | 15,970,304 | a well-formed **v2** container around a Mach-O kernel + Mach-O ramdisk |

What is *not* produced anywhere: an arm64 Linux `Image`, a boot image header
v3/v4, an AVB-signed image, a backup of any partition, or a UEFI for MT6877.

The shell truth for the build outputs (`stat` at build time):

```console
$ stat -f "%Sm | %z | %N" -t "%Y-%m-%d %H:%M:%S" hakios-kernel xnuloader/loader.efi qemu/payload.bin
2026-11-03 10:50:57 | 15938800 | hakios-kernel
2026-11-03 11:28:59 |    66048 | xnuloader/loader.efi
2026-11-03 11:29:56 | 15955552 | qemu/payload.bin
```

None of the three is tracked in git (`git ls-files --error-unmatch` fails on
each), so a fresh clone has the sources, not the artifacts.

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

`qemu/payload.bin` (rebuilt 2026-11-03 11:29, **15,955,552 bytes**) parses as:

| entry | bytes | content |
|---|---|---|
| `kernel` | 15,938,800 | Mach-O 64 |
| `ramdisk` | 16,528 | Mach-O 64 |
| `cmdline` | 45 | `-v cpus=8 bootcpu=7 virtio_gpu=1 gpu_trace=1` |
| `kernel.ver` | 35 | `2.1.13-hakios-darwin22.5-g22c7332b` |

(The entry list moves with every `make` target that rebuilds it — the earlier
revision of this table named a different cmdline and a `-dirty` version from a
different build. The *structure* is what matters; re-read with the parser below
rather than trusting this table's rows later.)

Reproduced here with the same inputs the Makefile uses:

```console
$ ./xnuloader/mkpayload.py -o /tmp/b651-payload.bin kernel=hakios-kernel \
    ramdisk=userland/hello --cmdline "-v cpus=8 bootcpu=7 fbdev=1 virtio_gpu=1"
/tmp/b651-payload.bin: 15955552 bytes
```

`mkpayload.py` needs only the kernel and the ramdisk; a payload can be built
without the phone, which is what makes it the one artifact that is genuinely
producible on demand.

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

`qemu/payload.bin` is written by the `make qemu-b651*` targets (a
`mkpayload.py` call) and consumed by `qemu/run-b651.sh`, which loads it through
QEMU's loader device at `0xF0000000`. It is a simulator artifact: nothing in
the phone reads an XNUP container, and nothing here writes one into a boot
image.

---

## 4. The gap, stated as fields

| header field | packer default | what the phone needs | source |
|---|---|---|---|
| `magic` | `ANDROID!` | same | certain |
| `header_version` | 0 (max supported: 2) | unknown; Android 14 A/B hardware commonly uses **3 or 4**, which the packer cannot emit (next table) | the phone's `boot_a` |
| `page_size` | 2048 | unknown: 2048 or 4096 | `boot_a` |
| `kernel_addr` | `0x40080000` | unknown | `boot_a` |
| `ramdisk_addr` | `0x41000000` | unknown | `boot_a` |
| `tags_addr` | `0x40000100` | unknown | `boot_a` |
| `dtb_addr` | `0x41F00000` | unknown; the phone's dtb is a separate `dtbo_a` and possibly `vendor_boot` | `boot_a`, `dtbo_a` |
| `cmdline` | empty | unknown; `/proc/cmdline` is denied to `shell` (PHONE.md) | BROM or root |
| `kernel` section | any file | **an arm64 Linux `Image`** — which does not exist for this port | not produced anywhere |
| `ramdisk` section | any file | on Android 13+, `init_boot_a` holds the generic ramdisk; `boot_a` may expect one, none, or a vendor ramdisk | `boot_a` size and contents |
| signature | none | AVB 1.2 is in force (`ro.boot.vbmeta.avb_version=1.2`, `verifiedbootstate=green`) | `boot_a` + `vbmeta` |

Two more header facts this file did not previously state, both verified against
the packer itself:

| | packer | what Android 14 / this phone may need | source |
|---|---|---|---|
| header versions | **0, 1, 2 only** — `--header-version 4` is rejected as an invalid choice | Android 13+ moved the generic ramdisk to `init_boot_a` and introduced boot header **v3/v4**, whose last section is a **boot signature**, not a dtb | `mkbootimg.py:708`, run below |
| AVB footer | **not emitted.** The output's last 64 bytes are zeroes; its size is exactly header page + section pages | a signed `boot` image carries an AVB footer (vbmeta blob) at the tail and/or a `vbmeta` partition entry pointing into it | `mkbootimg.py --info`, `xxd` tail |

The signature row deserves its own sentence: **a boot image without a valid AVB
signature will not boot on a locked device, whatever its header says.** An
unlocked `seccfg` changes that. `PHONE.md:104-116` reads the current unit as
locked with `sys.oem_unlock_allowed=0`, which means the toggle that would permit
one is greyed out. That is the gate, and it is above the format question.

So the packer is correct for the *container shape* and cannot express the two
things this build most likely has to carry: a header version past 2 and an AVB
signature. It is a format-complete v0/v1/v2 packer, not a v3/v4 or AVB packer.
Neither of those can be added by guessing — the version and the signing key are
both in `boot_a` / `avb_pkmd` / `vbmeta`, unread.

Also unnamed by any doc here: whether this phone keeps a generic ramdisk in
`init_boot_a` (Android 13+ layout, and the phone lists that partition) means a
HakiOS `boot_a` image may be the wrong partition to replace at all. The
partition table read in GATE1 step 3 answers that; it has not been done.

None of the rows above can be filled in from here. One is marked ASSUMED and
should be treated as a guess until read off the phone: `page_size=2048` is the
packer default and the usual MediaTek value, but nothing has confirmed it for
this board.

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

---

## 5a. The restore path, stated honestly

This section replaces the earlier claim that the restore side "is the one part
of shipping that is actually complete". It is not complete, and the reason is
not a missing script — `tools/restore.sh` is written and tested against
synthetic data. The reason is that **there is nothing to restore from, and no
verified way to write if there were.**

```text
route                      can we do it?                  evidence
-------------------------  -----------------------------  -----------------------------
back up over BROM          UNVERIFIED, never attempted    no *.img in the repo;
                                                          dumps/ = 2 text files
flash a custom image       NO on this locked unit         flash.locked=1,
   (fastboot)                                             oem_unlock_allowed=0 ->
                                                          toggle greyed, unsigned
                                                          refused
flash a custom image       WRONG-SHAPED, unsigned         AVB 1.2 green: LK
   (any route)                                            verifies boot_a
BROM (mtkclient / SP       UNVERIFIED, plausible          mtkclient ships a
   Flash Tool)                                            mt6877 payload
                                                          (SCOPE.md:47); no run
                                                          logged anywhere
restore                    CORRECT on synthetic data      restore.sh catches a
   (tools/restore.sh)      only; never on real dumps      MISMATCH and refuses
```

The three questions, answered one at a time:

**Is a fastboot flash possible at all?** Not on this unit as it reports itself.
`ro.boot.flash.locked=1` with `sys.oem_unlock_allowed=0` (`PHONE.md:104-116`)
means the OEM-unlocking switch is greyed out, so `fastboot flashing unlock` has
nothing to authorise, and the bootloader refuses an unsigned image. Whether LK
even implements `fastboot boot` is **unknown** — `GATE1.md:70-91` lists it as
one of three outcomes and none has been observed. `SCOPE.md:49-50` records that
MTK LK frequently refuses or hangs on a foreign image. This is a *reported
property*, not a measured one; the authoritative read is `seccfg`, unread.

**Is there a MediaTek BROM/preloader route, and does the device allow it?** A
route exists in principle — `mtkclient` (which ships an `mt6877` payload,
`SCOPE.md:47`) and SP Flash Tool both speak to MT6877 BootROM, and on this SoC
family BootROM normally exposes reads and writes without the Android lock state
mattering. **ASSUMED**: that general MTK behaviour has not been confirmed on
this unit, and some MTK parts enforce download-agent authentication / secure
boot, which would close it. **Nothing here has run it**: `GATE1.md:6-8` warns
that mtkclient's BROM handshake on macOS needs `libusb` and `sudo` and can hang
on Apple Silicon, and `GATE1.md:98-107` is written as commands to paste, not a
log. There is no recorded `mtk gettarget`, no `mt6877_payload.bin` invocation,
no dump. Whether this unit's BootROM accepts a download agent is **unverified**
and is the single fact that decides whether any restore is possible at all.

**Is a backup the only safety net, and has one been taken?** Yes, and no.
`ls -la devices/b651/dumps/` shows exactly two files —
`devicetree-nodes.txt` and `kernel.config.txt`, both derived from `/proc` — and
`find . -name '*.img'` returns nothing, with no `SHA256SUMS` anywhere. The
partitions that would have to be restored (`boot_a`, `lk_a`, `preloader_a`,
`seccfg`, and the unrecoverable `nvdata`/`nvram`/`proinfo`) have never been
read. So the honest statement is: **the one safety net is absent, not
untested.**

`restore.sh` itself is real, and its guarantees were re-checked here, not
taken from the older prose:

| case | result (re-run 2026-11-03) |
|---|---|
| `verify` with no backup | `0 verified, 1 failed`, exit 2 — it does not pass an empty directory |
| `verify --run`, hashes then `SHA256SUMS` recorded by `backup --run` | `6 verified, 0 failed` |
| one byte appended to `boot_a.img` | `boot_a MISMATCH ... 5 verified, 1 failed`, exit 2 |
| `restore --run` after that mismatch | prints `Refusing to plan a restore from a backup that did not verify.` |
| `backup` / `plan` / `restore` reach the phone? | no — every `mtk`/`fastboot`/`adb` line goes through `device_cmd()`, which only prints |

One caveat found while testing: `restore.sh backup --run` writes `SHA256SUMS`
with absolute paths (`sha256sum /tmp/synbackup/*.img`), and `verify` matches on
the `$2` field exactly. It works when the same `--backup` path is reused, which
is the documented flow; a backup directory that is copied elsewhere and then
verified will report every entry `UNRECORDED`. Not a bricking bug, but it means
"verify the copy you actually have" needs the hashes regenerated or the path
kept identical.

**What a wrong write costs.** `boot_a` is recoverable *if* a verified backup
exists and a write path exists. `preloader_a` is not: `restore.sh:221-224`
already says a wrong preloader write is unrecoverable without service hardware,
which is why it must never be flashed unless the phone is already dead. None of
the MTK "recovery of last resort" stories apply to a unit whose BROM acceptance
is unverified.

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

### The same packer, on the real kernel (re-run 2026-11-03)

The block above uses pattern files. This one uses what the build actually
produces, and is the image a person would be tempted to flash:

```console
$ python3 devices/b651/tools/mkbootimg.py --kernel hakios-kernel \
    --ramdisk userland/hello --devicetree /tmp/…/dtb --header-version 2 \
    -o /tmp/b651-verify/boot-real-v2.img --cmdline "-v cpus=8 bootcpu=7 fbdev=1 virtio_gpu=1"
/tmp/b651-verify/boot-real-v2.img: 15970304 bytes, header v2, page 2048
  kernel     15938800 bytes at 0x40080000 (7783 pages)
  ramdisk       16528 bytes at 0x41000000 (9 pages)
  dtb            9001 bytes at 0x41f00000 (5 pages)
  id       6513feb4b88e4715517bfa33bfc6e39a98dcec93

$ python3 devices/b651/tools/mkbootimg.py --info /tmp/b651-verify/boot-real-v2.img
...
kernel       15938800 B (15.20 MiB)     0x40080000    7783  Mach-O 64 (not a Linux kernel)
ramdisk         16528 B (0.02 MiB)     0x41000000       9  Mach-O 64 (not a Linux kernel)
```

The packer's own `--info` says the kernel section is "not a Linux kernel". The
container is correct and the payload inside it is the wrong kind of object —
which is §1's `kernel section` row, now measured rather than asserted.
`--unpack` returns the kernel byte-identical (`cmp` clean), so the packer loses
nothing; it is the *content* that LK cannot enter.

`mkbootimg.py --self-check` passes all nine cases (v0, v0+second, v1,
v2@4096, v2-all, 700-byte cmdline, section offsets 0/1/504, legacy MTK tag).

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
| all present and matching, `verify --run` | `6 verified, 0 failed`, exit 0 (re-run; the earlier figure of 7 counted a differently-shaped file set) |
| one file altered after hashing | `boot_a MISMATCH ... 1 failed`, exit 2 |
| file missing that the slot does not need | reported `MISSING`, not a failure |
| `restore` after a failed verify | prints `Refusing to plan a restore from a backup that did not verify.` |
| `restore --slot b` where `boot_b` verified | prints the whole plan, exit 0 |
| default (no `--run`) | hashes are not computed; says so and exits 2 |
| `--slot c`, missing `--backup` | usage error, exit 1 |

Audit that nothing in it can reach a device: every `mtk` / `fastboot` / `adb`
line goes through `device_cmd()`, which only `say`s it. Grepping for those
binaries outside comments and `device_cmd` returns nothing.

**Re-run 2026-11-03, and two corrections to the older table above.** The run
used random 4 KiB files and `backup --run` to generate `SHA256SUMS`:

```console
$ bash devices/b651/tools/restore.sh verify --backup /tmp/synbackup --run
6 verified, 0 failed                                    # after backup --run recorded the hashes
$ printf 'X' >> /tmp/synbackup/boot_a.img && ... verify --run
boot_a           MISMATCH     4097       d0370a6d...
5 verified, 1 failed                                    # exit 2
$ .../restore.sh restore --backup /tmp/synbackup --run
Refusing to plan a restore from a backup that did not verify.
$ rm -rf <all images>; .../restore.sh verify --backup /tmp/emptybackup --run
0 verified, 1 failed. Nothing was proved...              # exit 2, does not pass an empty dir
```

| correction | detail |
|---|---|
| the summary line counts `MISSING` for a required slot as a fail, so an empty directory is correctly `0 verified, 1 failed` | the old row "7 verified" counted a file set that included one it did not actually require |
| `verify` did **not** confirm hashes when `SHA256SUMS` was written with absolute paths by a different tool | `backup --run` writes `sha256sum /tmp/synbackup/*.img`; `verify` matches `$2` exactly. Reuse the same `--backup` path, or every row reads `UNRECORDED` |

There is no CI test for `restore.sh` (`grep -rn restore.sh ci/ Makefile` returns
nothing), so these are the only checks that exist.

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
| boot header v3/v4 and AVB signing | not implemented in the packer; version and key are in unread partitions |
| **whether BROM/DA accepts a download on this unit** | never run; decides whether any restore is possible |
| **whether a backup exists** | it does not — no partition has been read |

What *is* verified: the packer produces correctly structured v0/v1/v2 images and
reads them back byte-identically (including the real kernel), its `id` matches
AOSP's rule, and the restore script is syntactically valid, correct against a
synthetic backup, and contains no path that writes to a phone.

What is *not* verified, and was previously implied to be: that the restore side
is complete. It is a correct procedure with nothing to operate on.

---

## 8. The order that would actually ship this

Each step produces evidence the next one needs; none of them is a build. **A
step marked "yes" needs the phone and has never been run.**

| # | Step | Produces | Needs phone | Run yet? |
|---|---|---|---|---|
| 0 | confirm BROM/DA accepts a download at all (`mtk gettarget`) | whether *any* read or restore is possible | yes | **no** |
| 1 | `mtk r boot_a boot_a.img`, then `mkbootimg.py --info boot_a.img` | the header dialect, page size, every address, the cmdline, whether a ramdisk is present | yes | **no** |
| 2 | `mtk r seccfg` and a lock-state read (GATE1 step 6) | whether an unsigned image can run at all | yes | **no** |
| 3 | dump **every** partition in `restore.sh`'s list, then `restore.sh verify --run` | a backup that exists, and a restore path checked against hashes taken at dump time | yes | **no** |
| 4 | a Linux arm64 `Image` that can start the loader (or a UEFI that can) | the missing link in §1 | no | no — does not exist |
| 5 | a v3/v4 + AVB-signed image, if `boot_a` turns out to be either | the container the phone's LK would actually verify | partly | no |
| 6 | read-only try: `fastboot boot` (RAM only), if LK has it | whether LK runs it, with nothing written | yes | **no** |

Step 0 is now the cheapest and unblocks the most; step 1 cannot happen until it
does. Until they happen, **this directory can produce a well-formed boot image,
not a bootable one, and it cannot produce a way back from a bad write**, and it
says so because the alternative is a file that looks right and bricks a phone.
