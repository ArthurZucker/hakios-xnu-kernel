#!/usr/bin/env python3
"""
mkbootimg.py - pack a kernel and a ramdisk into an Android boot image, the
container MediaTek's LK (the preloader-era "little kernel") parses, and read one
back apart. Standard library only; no Android tools are needed.

  pack:    mkbootimg.py --kernel Image --ramdisk ramdisk.cpio.gz -o boot.img
  info:    mkbootimg.py --info boot.img
  unpack:  mkbootimg.py --unpack boot.img -o outdir

The image is a header, then the sections, each rounded up to a page boundary:

  header version 0                     every section starts on a page boundary
  +------------------------------+
  | boot header         1 page   |     1632 bytes (v0), 1648 (v1), 1660 (v2)
  +------------------------------+
  | kernel             n pages   |     n = ceil(kernel_size / page_size)
  +------------------------------+
  | ramdisk            m pages   |     m = ceil(ramdisk_size / page_size)
  +------------------------------+
  | second             o pages   |     o = 0 when second_size == 0
  +------------------------------+
  | recovery dtbo      p pages   |     v1/v2 only; 0 when recovery_dtbo_size == 0
  +------------------------------+
  | dtb                q pages   |     v2 only
  +------------------------------+

Every number in the header is a little-endian u32 unless the comment says
otherwise. Offsets are from the start of the header (offset 0 of the image):

  off  size  field                 meaning
  ---- ----  --------------------- ------------------------------------------------
   0      8  magic                 "ANDROID!" - the only thing LK looks for first
   8      4  kernel_size           bytes of kernel, as stored (including any MTK
                                   sub-header, see --mtk-header)
  12      4  kernel_addr           physical address the kernel is copied to
  16      4  ramdisk_size          bytes of ramdisk, as stored
  20      4  ramdisk_addr          physical address the ramdisk is copied to
  24      4  second_size           bytes of second stage; 0 = no second stage
  28      4  second_addr           physical address of the second stage
  32      4  tags_addr             physical address for the ATAG/FDT the kernel
                                   reads its cmdline from on 32-bit; on arm64 the
                                   value is carried but the FDT is passed in x0
  36      4  page_size             alignment of every section; 2048 on most
                                   MediaTek boards, 4096 on some
  40      4  header_version        ** see "the two dialects" below **
  44      4  os_version            A[31:25] B[24:18] C[17:11] (Y-2000)[10:4] M[3:0]
  48     16  name                  product name, NUL padded
  64    512  cmdline               kernel command line, NUL padded
 576     32  id                    SHA-1 over the sections (see id_bytes() below)
 608   1024  extra_cmdline         the tail of a command line longer than 511

  v1 (>= 1648): 1632 + 4 recovery_dtbo_size, + 8 recovery_dtbo_offset,
                + 4 header_size (= 1648)
  v2 (>= 1660): 1648 + 4 dtb_size, + 8 dtb_addr

The two dialects at offset 40
-----------------------------
The 8 bytes at 40 are two different fields depending on the image's age, and
nothing in the image says which:

  pre-2018 (old AOSP mkbootimg, and the MediaTek forks):
      40  u32 dt_size      size of a device tree stored after `second`
      44  u32 unused       must be 0
  2018+ (AOSP boot image header v0 and later):
      40  u32 header_version
      44  u32 os_version

A reader that trusts hdr.header_version on an old MediaTek image sees dt_size,
which is the size of a page-aligned device tree - a number that is not a valid
header version. This script treats anything above 2 in that field as an old
image and says so (`--info`), which is the cheapest way to tell them apart
without the phone's own boot_a.

Defaults, and why they are only defaults
----------------------------------------
0x40000000 / kernel + 0x00080000 / ramdisk + 0x01000000 / tags + 0x00000100 are
the usual MediaTek arm64 addresses, but this phone's real values are in its own
boot_a header, which has not been read (see ../SHIP.md). Pass --base,
--kernel-offset, --ramdisk-offset, --tags-offset, --devicetree-load-addr and --page-size
once they are known. LK copies sections to the addresses in the header; a wrong
kernel_addr is a phone that does not boot, not a build error.

--mtk-header adds MediaTek's legacy 512-byte tagged sub-header in front of the
kernel and the ramdisk (magic 88 16 88 58, the section size, then "KERNEL" or
"ROOTFS"). It was current on MT6582/MT6592-era phones and is not expected on
Android 14 hardware; it is here because "MediaTek format" means this to some
tools, and because both directions are testable offline. See mtk_subheader().
"""

import argparse
import hashlib
import os
import struct
import sys

BOOT_MAGIC = b'ANDROID!'
MTK_MAGIC = b'\x88\x16\x88\x58'
NAME_SIZE, ARGS_SIZE, EXTRA_ARGS_SIZE = 16, 512, 1024
ID_SIZE = 32

# 8 magic + 8 u32 (kernel_size..page_size) + 2 u32 (header_version, os_version)
# + name + cmdline + id + extra_cmdline
V0_SIZE = 8 + 8 * 4 + 2 * 4 + NAME_SIZE + ARGS_SIZE + ID_SIZE + EXTRA_ARGS_SIZE   # 1632
EXTRA_SIZE = {0: 0, 1: 16, 2: 28}      # v1: dtbo size/offset + header_size; v2: + dtb size/addr

DEFAULT_BASE = 0x40000000
DEFAULT_KERNEL_OFFSET = 0x00080000
DEFAULT_RAMDISK_OFFSET = 0x01000000
DEFAULT_SECOND_OFFSET = 0x00F00000
DEFAULT_TAGS_OFFSET = 0x00000100
DEFAULT_DTB_ADDR = 0x41F00000
DEFAULT_PAGE_SIZE = 2048


def header_size(version):
    if version not in EXTRA_SIZE:
        raise SystemExit(f'mkbootimg.py: unsupported header version {version} (0, 1 or 2)')
    return V0_SIZE + EXTRA_SIZE[version]


def align(n, page):
    return (n + page - 1) // page * page


def read_file(path):
    with open(path, 'rb') as f:
        return f.read()


def mtk_subheader(name, size):
    """MediaTek's legacy 512-byte tag: magic, the section size, an ASCII name, 0x00 to 40, 0xFF after.

    The magic is written in the order 88 16 88 58 (the u32 0x58881688 little-endian).
    The size at offset 4 is written little-endian by the reference implementation
    (bkerler/Archive385 mkbootimg's mtk_load_file(): it sprintf()s the size as hex
    and sscanf()s it back into a char[4], which stores the int's bytes as-is on a
    little-endian host). That is reproduced here deliberately, not derived from a
    MediaTek document; it has not been checked against a real MediaTek image.
    """
    if len(name) > 32:
        raise SystemExit(f'mkbootimg.py: MTK tag {name!r} is longer than 32 bytes')
    tag = bytearray(MTK_MAGIC)
    tag += struct.pack('<I', size)
    tag += name.encode().ljust(32, b'\0')                      # ends at 40
    tag += b'\0' * (40 - len(tag))                             # no-op, kept explicit
    tag += b'\xff' * (512 - 40)
    assert len(tag) == 512
    return bytes(tag)


def id_bytes(version, kernel, ramdisk, second, dtb=None):
    """The header `id`: SHA-1 over each section followed by that section's size, as u32 LE.

    This is AOSP's own rule (system/tools/mkbootimg/mkbootimg.py, update_sha()):
    every section is fed in as content + its size, and a section that is not there
    is fed in as the single u32 0 - not skipped. v1 and later therefore also hash
    the (empty) recovery_dtbo, and v2 hashes the dtb after it. The header carries
    the first 20 bytes of this SHA-1 in a 32-byte field.

    It is a consistency check, not a signature: it proves the sections are the ones
    the header was made for and nothing changed afterwards.
    """
    h = hashlib.sha1()
    for data in (kernel, ramdisk, second):
        h.update(data)
        h.update(struct.pack('<I', len(data)))
    if version >= 1:
        h.update(struct.pack('<I', 0))              # empty recovery_dtbo, as AOSP does
    if version >= 2:
        h.update(dtb or b'')
        h.update(struct.pack('<I', len(dtb or b'')))
    return h.digest()


def build_header(version, page_size, kernel, ramdisk, second, dtb,
                 kernel_addr, ramdisk_addr, second_addr, tags_addr,
                 dtb_addr, name, cmdline):
    cmd = cmdline.encode()
    if len(cmd) < ARGS_SIZE:                       # AOSP: the 511th byte is dropped
        first, extra = cmd.ljust(ARGS_SIZE, b'\0'), b''
    else:
        first = (cmd[:ARGS_SIZE - 1]).ljust(ARGS_SIZE, b'\0')
        extra = cmd[ARGS_SIZE - 1:][:EXTRA_ARGS_SIZE]
    extra = extra.ljust(EXTRA_ARGS_SIZE, b'\0')

    h = bytearray()
    h += BOOT_MAGIC
    h += struct.pack('<8I', len(kernel), kernel_addr, len(ramdisk), ramdisk_addr,
                     len(second), second_addr, tags_addr, page_size)
    h += struct.pack('<II', version, 0)                       # header_version, os_version
    h += name.encode()[:NAME_SIZE].ljust(NAME_SIZE, b'\0')
    h += first
    # AOSP copies one SHA-1 (20 bytes) into the 32-byte field and leaves the rest zero.
    h += id_bytes(version, kernel, ramdisk, second, dtb).ljust(ID_SIZE, b'\0')
    h += extra
    if version >= 1:
        h += struct.pack('<IQI', 0, 0, header_size(version))  # no recovery dtbo; header_size
    if version == 2:
        h += struct.pack('<IQ', len(dtb), dtb_addr)
    assert len(h) == header_size(version), (len(h), header_size(version))
    return bytes(h)


def cmd_pack(args):
    kernel = read_file(args.kernel)
    ramdisk = b'' if args.no_ramdisk else read_file(args.ramdisk)
    second = read_file(args.second) if args.second else b''
    dtb = read_file(args.dtb) if args.dtb else b''
    if args.dtb and args.header_version != 2:
        raise SystemExit('mkbootimg.py: --devicetree needs --header-version 2 (in v0/v1 a device tree '
                         'goes in the `second` section, or in vendor_boot on newer phones)')
    if not ramdisk and not args.no_ramdisk:
        raise SystemExit('mkbootimg.py: --ramdisk is required (or --no-ramdisk, header v0/v1 only)')
    if args.no_ramdisk and args.header_version == 2:
        raise SystemExit('mkbootimg.py: header v2 requires a ramdisk')

    if args.mtk_header:
        if not kernel:
            raise SystemExit('mkbootimg.py: --mtk-header needs a kernel')
        kernel = mtk_subheader('KERNEL', len(kernel)) + kernel
        if ramdisk:
            ramdisk = mtk_subheader('ROOTFS', len(ramdisk)) + ramdisk

    addr = lambda off: args.base + off
    hdr = build_header(args.header_version, args.page_size, kernel, ramdisk, second, dtb,
                       addr(args.kernel_offset), addr(args.ramdisk_offset),
                       addr(args.second_offset), addr(args.tags_offset),
                       args.dtb_addr, args.board, args.cmdline)

    out = bytearray(hdr.ljust(args.page_size, b'\0'))
    for section in (kernel, ramdisk, second, dtb):
        if section:
            out += section.ljust(align(len(section), args.page_size), b'\0')
    with open(args.output, 'wb') as f:
        f.write(out)

    print(f'{args.output}: {len(out)} bytes, header v{args.header_version}, page {args.page_size}')
    for label, data, a in (('kernel', kernel, addr(args.kernel_offset)),
                           ('ramdisk', ramdisk, addr(args.ramdisk_offset)),
                           ('second', second, addr(args.second_offset)),
                           ('dtb', dtb, args.dtb_addr if args.header_version == 2 else None)):
        if data:
            print(f'  {label:8s} {len(data):>10d} bytes at 0x{a:08x} '
                  f'({align(len(data), args.page_size) // args.page_size} pages)')
    print(f'  id       {id_bytes(args.header_version, kernel, ramdisk, second, dtb).hex()}')


def parse(data):
    """Read a header back out of `data`. Returns a dict; raises SystemExit if there is none."""
    off = 0
    if data[:8] != BOOT_MAGIC:
        for i in range(1, 513):                 # unpackbootimg scans the first 512 bytes too
            if data[i:i + 8] == BOOT_MAGIC:
                off = i
                break
        else:
            raise SystemExit('mkbootimg.py: no "ANDROID!" magic in the first 512 bytes: '
                             'this is not a boot image')
    if off + V0_SIZE > len(data):
        raise SystemExit(f'mkbootimg.py: header truncated ({len(data)} bytes)')

    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr,
     tags_addr, page_size) = struct.unpack_from('<8I', data, off + 8)
    word40, word44 = struct.unpack_from('<II', data, off + 40)
    name = data[off + 48:off + 64].split(b'\0')[0].decode('utf-8', 'replace')
    cmdline = data[off + 64:off + 576].split(b'\0')[0].decode('utf-8', 'replace')
    extra = data[off + 608:off + 1632].split(b'\0')[0].decode('utf-8', 'replace')

    # 40 is header_version on 2018+ images and dt_size on old ones. Only 0..2 are
    # versions this script writes; anything else is almost certainly a dt_size.
    legacy = word40 > 2
    version = 0 if legacy else word40
    h = {'offset': off, 'version': version, 'legacy_dt': legacy,
         'word40': word40, 'word44': word44, 'dt_size_guess': word40 if legacy else None,
         'kernel_size': kernel_size, 'kernel_addr': kernel_addr,
         'ramdisk_size': ramdisk_size, 'ramdisk_addr': ramdisk_addr,
         'second_size': second_size, 'second_addr': second_addr,
         'tags_addr': tags_addr, 'page_size': page_size,
         'os_version': 0 if legacy else word44, 'name': name,
         'cmdline': (cmdline + extra), 'size': len(data)}

    if not legacy and version in (1, 2) and off + header_size(version) <= len(data):
        (rec_size,) = struct.unpack_from('<I', data, off + V0_SIZE)
        rec_off = struct.unpack_from('<Q', data, off + V0_SIZE + 4)[0]
        (hdr_size,) = struct.unpack_from('<I', data, off + V0_SIZE + 12)
        h.update(recovery_dtbo_size=rec_size, recovery_dtbo_offset=rec_off, header_size=hdr_size)
        if rec_size:
            h['recovery_dtbo'] = data[off + rec_off:off + rec_off + rec_size]
    if not legacy and version == 2 and off + header_size(2) <= len(data):
        dsize, daddr = struct.unpack_from('<IQ', data, off + V0_SIZE + 16)
        h.update(dtb_size=dsize, dtb_addr=daddr)

    page = page_size or DEFAULT_PAGE_SIZE
    # v0 and the old dialect both have a 1632-byte header; the sections start at the next page.
    p = (off + header_size(version) + page - 1) // page * page
    h['sections'] = {}
    for key, size in (('kernel', kernel_size), ('ramdisk', ramdisk_size),
                      ('second', second_size), ('recovery_dtbo', h.get('recovery_dtbo_size', 0)),
                      ('dtb', h.get('dtb_size', 0))):
        if not size:
            continue
        if p + size > len(data):
            h['truncated'] = key
            break
        h['sections'][key] = data[p:p + size]
        p += align(size, page)
    return h


def describe(h):
    page = h['page_size']
    dur = lambda n: f'{n} B' if n < 4096 else f'{n} B ({n / 1048576:.2f} MiB)'
    print(f'image        {h["size"]} bytes')
    print(f'magic        at offset {h["offset"]}')
    print(f'page_size    {page}')
    if h['legacy_dt']:
        print(f'header       OLD dialect: the u32 at offset 40 is dt_size = {h["dt_size_guess"]}, '
              f'not a header version')
    else:
        print(f'header       version {h["version"]}'
              + (f' (header_size field {h.get("header_size")})' if 'header_size' in h else ''))
    print(f'os_version   0x{h["os_version"]:08x}')
    print(f'name         {h["name"]!r}')
    print(f'cmdline      {h["cmdline"]!r}')
    print()
    print(f'{"section":12s} {"size":>14s} {"load address":>14s} {"pages":>7s}  magic')
    for key, size, addr in (('kernel', h['kernel_size'], h['kernel_addr']),
                            ('ramdisk', h['ramdisk_size'], h['ramdisk_addr']),
                            ('second', h['second_size'], h['second_addr']),
                            ('recovery_dtbo', h.get('recovery_dtbo_size', 0), None),
                            ('dtb', h.get('dtb_size', 0), h.get('dtb_addr'))):
        if not size:
            continue
        data = h['sections'].get(key)
        tag = ''
        if data:
            if data[:4] == MTK_MAGIC:
                tag = 'MTK 512-byte tag, real data after it'
            elif data[0x38:0x3c] == b'ARM\x64':
                tag = 'Linux arm64 Image (ARM\\x64)'
            elif data[:4] == b'\xcf\xfa\xed\xfe':
                tag = 'Mach-O 64 (not a Linux kernel)'
            elif data[:2] == b'\x1f\x8b':
                tag = 'gzip'
            else:
                tag = data[:4].hex()
        print(f'{key:12s} {dur(size):>14s} '
              f'{("0x%08x" % addr) if addr else "-":>14s} '
              f'{align(size, page) // page:>7d}  {tag}')
    if 'truncated' in h:
        print(f'\nwarning: the image is shorter than the header says ({h["truncated"]} runs past the end)')
    print(f'\ntags_addr    0x{h["tags_addr"]:08x}')
    print(f'second_addr  0x{h["second_addr"]:08x}')


def cmd_info(args):
    describe(parse(read_file(args.info)))


def cmd_unpack(args):
    h = parse(read_file(args.unpack))
    os.makedirs(args.output, exist_ok=True)
    written = []
    for key, data in h['sections'].items():
        if key == 'kernel' and data[:4] == MTK_MAGIC:
            continue                                   # the tag is described, not extracted
        path = os.path.join(args.output, key)
        with open(path, 'wb') as f:
            f.write(data)
        written.append((path, data))
    with open(os.path.join(args.output, 'cmdline'), 'w') as f:
        f.write(h['cmdline'] + '\n')
    with open(os.path.join(args.output, 'header.txt'), 'w') as f:
        for k, v in sorted(h.items()):
            if k == 'sections':
                v = {n: len(d) for n, d in v.items()}
            f.write(f'{k} {v}\n')
    print(f'{args.unpack} -> {args.output}/: {", ".join(os.path.basename(p) for p, _ in written)}')
    for path, data in written:
        print(f'  {os.path.basename(path):10s} {len(data):>10d} bytes  sha256 {hashlib.sha256(data).hexdigest()}')


def main():
    ap = argparse.ArgumentParser(
        description='Pack or unpack an Android/MediaTek boot image (stdlib only).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split('The two dialects')[0].strip())
    ap.add_argument('--kernel', help='kernel (arm64 Image, or gzip of it)')
    ap.add_argument('--ramdisk', help='ramdisk')
    ap.add_argument('--no-ramdisk', action='store_true', help='header v0/v1 only: a zero ramdisk')
    ap.add_argument('--second', help='second stage')
    ap.add_argument('--devicetree', dest='dtb',
                    help='a device tree as its own section (header v2 only; in v0/v1 it is in `second`)')
    ap.add_argument('--cmdline', default='', help='kernel command line')
    ap.add_argument('--board', default='', help='product name, 16 bytes max')
    ap.add_argument('-o', '--output', help='the image to write (pack), or the directory (--unpack)')
    ap.add_argument('--header-version', dest='header_version', type=int, default=0, choices=(0, 1, 2))
    ap.add_argument('--page-size', dest='page_size', type=int, default=DEFAULT_PAGE_SIZE,
                    choices=(2048, 4096, 8192, 16384))
    ap.add_argument('--base', type=lambda s: int(s, 0), default=DEFAULT_BASE)
    ap.add_argument('--kernel-offset', dest='kernel_offset', type=lambda s: int(s, 0), default=DEFAULT_KERNEL_OFFSET)
    ap.add_argument('--ramdisk-offset', dest='ramdisk_offset', type=lambda s: int(s, 0), default=DEFAULT_RAMDISK_OFFSET)
    ap.add_argument('--second-offset', dest='second_offset', type=lambda s: int(s, 0), default=DEFAULT_SECOND_OFFSET)
    ap.add_argument('--tags-offset', dest='tags_offset', type=lambda s: int(s, 0), default=DEFAULT_TAGS_OFFSET)
    ap.add_argument('--devicetree-load-addr', dest='dtb_addr', type=lambda s: int(s, 0), default=DEFAULT_DTB_ADDR)
    ap.add_argument('--mtk-header', dest='mtk_header', action='store_true',
                    help='legacy 512-byte MediaTek tag in front of kernel and ramdisk')
    ap.add_argument('--info', metavar='IMG', help='print the header of IMG and stop')
    ap.add_argument('--unpack', metavar='IMG', help='write the sections of IMG into -o DIR')
    args = ap.parse_args()

    if args.info:
        cmd_info(args)
    elif args.unpack:
        if not args.output:
            raise SystemExit('mkbootimg.py: --unpack needs -o DIR')
        cmd_unpack(args)
    else:
        if not args.kernel or not args.output:
            ap.error('packing needs --kernel and -o (or --info/--unpack)')
        if not args.ramdisk and not args.no_ramdisk:
            ap.error('--ramdisk is required (or --no-ramdisk)')
        cmd_pack(args)


if __name__ == '__main__':
    main()
