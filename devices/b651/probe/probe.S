/*
 * HakiOS gate-1 probe -- B651 (Bigme HiBreak Pro, MediaTek MT6877)
 *
 * Answers exactly one question: "does this phone run foreign unsigned arm64
 * code at all?"  It writes a banner to UART0 and spins.  It writes nothing
 * else: no flash, no MMIO other than the UART FIFO/LSR.
 *
 * Freestanding: no libc, no stack use, no relocations, no MMU.  Physical
 * addressing throughout, so it works at whatever address ABL/LK loads it.
 *
 * Header: 64-byte arm64 "Image" header, as consumed by the arm64 boot
 * protocol (Documentation/arch/arm64/booting.rst):
 *
 *   off  size  field          value here
 *   0x00  4    code0          `b entry`  -- real branch, offset (+64)
 *   0x04  4    code1          `nop`      (was the second instruction word)
 *   0x08  8    text_offset    0x0
 *   0x10  8    image_size     total flat image size, set at link time
 *   0x18  8    flags          0  (LE, 4K pages, kernel at text_offset)
 *   0x20  8    res2           0
 *   0x28  8    res3           0
 *   0x30  8    res4           0
 *   0x38  4    magic          0x644d5241  ("ARM\x64", LE)
 *   0x3c  4    res5           0
 *
 * Build-time knobs (see build.sh):
 *   UART_BASE   physical base of the UART (default MT6877 UART0 0x11002000)
 *   UART_PL011  1 => PL011 register layout (QEMU virt @ 0x09000000)
 *               0 => 16550 layout, 32-bit stride-4 regs (MT6577/8250_mtk)
 */

#ifndef UART_BASE
#define UART_BASE 0x11002000            /* MT6877 apuart0 */
#endif
#ifndef UART_PL011
#define UART_PL011 0                    /* 0 = MT6577/16550, 1 = PL011 */
#endif

/* ---- 64-byte arm64 Image header ------------------------------------- */
    /* Header and code share one .text so the linker cannot reorder the
     * header away from offset 0 (a .text.header section is treated as an
     * orphan by lld and lands after .text).  Source order is preserved
     * inside a single section. */
    .text
    .globl  _start
_start:
    b       entry                   /* code0: branch to entry */
    nop                             /* code1 */
    .quad   0                       /* text_offset */
    .quad   __image_end - _start    /* image_size (linked) */
    .quad   0                       /* flags */
    .quad   0                       /* res2 */
    .quad   0                       /* res3 */
    .quad   0                       /* res4 */
    .long   0x644d5241              /* magic */
    .long   0                       /* res5 */
    /* 64 bytes exactly -- assert in build.sh */

/* ---- entry ----------------------------------------------------------- */
    .globl  entry
entry:
    /* x0 may hold a DTB pointer from ABL.  We do not need it: no parsing,
     * no memory writes, nothing that could fault on a bad/absent DTB.     */
    mov     x19, x0                 /* kept only for the record */

    adrp    x1, banner
    add     x1, x1, :lo12:banner

1:  ldrb    w2, [x1], #1
    cbz     w2, 2f
    bl      uart_putc
    b       1b

2:  /* spin forever; wfe keeps the core quiet (and works with no GIC) */
3:  wfe
    b       3b

/* --------------------------------------------------------------------- */
/* void uart_putc(w2 = byte)
 * Clobbers x3..x6.  Bounded poll on the transmitter-empty bit so a dead or
 * unclocked UART cannot wedge the CPU in the print loop forever.          */
uart_putc:
#if UART_PL011
    /* PL011 (QEMU virt): DR @0x00, FR @0x18, TXFE = bit 7, 32-bit regs. */
    movz    x3, #(UART_BASE & 0xffff), lsl #0
  #if (UART_BASE >> 16) & 0xffff
    movk    x3, #((UART_BASE >> 16) & 0xffff), lsl #16
  #endif
    mov     w6, #0x10000            /* spin bound */
4:  ldr     w4, [x3, #0x18]          /* FR */
    tbz     w4, #7, 5f              /* TXFE clear -> fifo full, wait */
    str     w2, [x3, #0x00]         /* DR */
    ret
5:  subs    w6, w6, #1
    b.ne    4b
    ret                             /* gave up: drop the byte */
#else
    /* 16550 / 8250_mtk (MT6577-uart): THR @0x00, LSR @0x14, THRE = bit 5.
     * MediaTek's 8250 variant uses one 32-bit register per byte offset
     * (UPIO_MEM32), hence the str/ldr of w at [base + reg*4].             */
    movz    x3, #(UART_BASE & 0xffff), lsl #0
  #if (UART_BASE >> 16) & 0xffff
    movk    x3, #((UART_BASE >> 16) & 0xffff), lsl #16
  #endif
    mov     w6, #0x10000            /* spin bound */
4:  ldr     w4, [x3, #0x14]          /* LSR */
    tbz     w4, #5, 5f              /* THRE clear -> hold off */
    str     w2, [x3, #0x00]         /* THR */
    ret
5:  subs    w6, w6, #1
    b.ne    4b
    ret                             /* gave up: drop the byte */
#endif

/* --------------------------------------------------------------------- */
/* Banner lives in .text (not .rodata) so that __image_end - _start is
 * exactly the flat file size: one section, no gaps, nothing after it.   */
banner:
    .asciz "\r\nHAKIOS PROBE: arm64 image ran\r\n"

    .balign 16
__image_end:
