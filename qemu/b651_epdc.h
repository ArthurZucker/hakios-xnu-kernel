/*
 * b651_epdc.h: the register file of the HiBreak Pro's e-ink controller, as a QEMU device.
 *
 * The .c next to this has the model; this file has only what a second file would need to talk to it -- the QOM
 * type name, the register offsets, and the bits of the status register. Everything here is OURS for the
 * simulator: the phone's EPDC base address and register layout are unread (devices/b651/EPDC.md:157-160,
 * HARDWARE.md:57-58 says there is no `epdc`/`eink` node in any MT6877 tree), so these numbers are a
 * convention of the model and must not be built into the kernel driver. b651_epdc.c's head comment says
 * which parts of the semantics ARE taken from the repo's documents, and which are a model.
 *
 * The layout mirrors what mxc_epdc_fb.c programs before a LUT starts (EPDC.md:29-36): a panel geometry,
 * a waveform/update mode, one damage rectangle, a trigger, and a status the caller polls or waits on.
 */
#ifndef B651_EPDC_H
#define B651_EPDC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_B651_EPDC "b651-epdc"

OBJECT_DECLARE_SIMPLE_TYPE(B651EpdcState, B651_EPDC)

/*
 * The register window. 4 KiB, one register per 4 bytes, no aliasing: a real controller has far more state
 * (LUT banks, timing, collision masks -- EPDC.md:47), and this file exposes only what the driver's state
 * machine and a viewer use (EPDC.md:115-122).
 */
#define B651_EPDC_MMIO_SIZE       0x1000

#define B651_EPDC_REG_PANEL_W     0x000     /* ro: panel width in pixels (1125; SIM.md:195-197) */
#define B651_EPDC_REG_PANEL_H     0x004     /* ro: panel height in pixels (2436; same source) */
#define B651_EPDC_REG_LEVELS      0x008     /* ro: grey levels the grey waveforms address (16, ASSUMED:
                                             * WAVEFORMS.md section 2.3 and 3.6) */
#define B651_EPDC_REG_MODE        0x00c     /* rw: waveform to drive with on the next trigger. The four values
                                             * are an INDEX into the device's waveform table, not Bigme's
                                             * Android integers (4/132/178/180/1029), whose mapping is an open
                                             * question (WAVEFORMS.md section 1.3). */
#define B651_EPDC_REG_DAMAGE_X0   0x010
#define B651_EPDC_REG_DAMAGE_Y0   0x014
#define B651_EPDC_REG_DAMAGE_X1   0x018     /* exclusive, as QEMU's own rects are */
#define B651_EPDC_REG_DAMAGE_Y1   0x01c     /* exclusive */
#define B651_EPDC_REG_TRIGGER     0x020     /* w: any non-zero write starts the update the registers above
                                             * describe. Reads back 0. */
#define B651_EPDC_REG_STATUS      0x024     /* ro: B651_EPDC_STATUS_* below */
#define B651_EPDC_REG_FRAMES      0x028     /* ro: frame count of the update that is running, or of the last
                                             * one that finished, 0 before the first */
#define B651_EPDC_REG_PARTIAL     0x02c     /* ro: partial updates since the last flashing one. The number
                                             * WAVEFORMS.md section 4 row 8 says a faithful model owes its
                                             * caller -- the caller decides when to clear, the device does not. */
#define B651_EPDC_REG_TEMP        0x030     /* rw: degrees Celsius, stored and NOT used to pick a waveform
                                             * (WAVEFORMS.md section 4 row 5: no invented temperature curve). */

#define B651_EPDC_STATUS_BUSY       (1u << 0)   /* the waveform's frames have not all elapsed */
#define B651_EPDC_STATUS_COLLISION  (1u << 1)   /* a trigger arrived while busy and was refused. Sticky:
                                                 * cleared by the next trigger that is accepted. */

/* B651_EPDC_REG_MODE's four values. WAVEFORMS.md section 1.1 is what each one means. */
#define B651_EPDC_MODE_GC16        0
#define B651_EPDC_MODE_GL16        1
#define B651_EPDC_MODE_DU          2
#define B651_EPDC_MODE_A2          3
#define B651_EPDC_MODE_COUNT       4

/*
 * The one address the model claims, and the one thing about it that must be said plainly: this is a simulator
 * convention. QEMU's `virt` machine has no device at 0x0f000000 -- its GIC is at 0x08000000, its virtio-mmio
 * window at 0x0a000000, and its PCI MMIO window starts at 0x10000000, while guest RAM starts at 0x40000000 --
 * so a 4 KiB register page plus a 2.7 MB framebuffer fits in a hole and collides with nothing. It is NOT the
 * phone's EPDC base, which nobody has read (EPDC.md:157). The device takes it as the `mmio-base` property, so
 * `-global b651-epdc.mmio-base=...` moves both windows without an edit.
 */
#define B651_EPDC_DEFAULT_MMIO_BASE  0x0f000000u

/* The framebuffer window sits this far above the register page: one 4 KiB page, so a single base property
 * places both. */
#define B651_EPDC_FB_OFFSET        B651_EPDC_MMIO_SIZE

typedef enum B651EpdcWaveformMode {
    B651_EPDC_WAVE_GC16 = B651_EPDC_MODE_GC16,
    B651_EPDC_WAVE_GL16 = B651_EPDC_MODE_GL16,
    B651_EPDC_WAVE_DU   = B651_EPDC_MODE_DU,
    B651_EPDC_WAVE_A2   = B651_EPDC_MODE_A2,
} B651EpdcWaveformMode;

/*
 * What the panel is actually showing: one byte per pixel, 0x22..0xDD (ink, not a request), stride == width, no
 * padding. This is the viewer's entry point -- qemu/epd.py, or `screendump` of a console that reads it -- and it
 * is a *result*, so what a viewer paints is what the ink holds rather than what the guest asked for. Mid-update
 * it is a frame of the waveform, which is why a gc16 refresh is watchable as a blink. The pointer is into the
 * device's own buffer and is only stable until the next frame: copy it, do not keep it.
 */
const uint8_t *b651_epdc_panel_state(const B651EpdcState *s, uint32_t *width, uint32_t *height);

#endif /* B651_EPDC_H */
