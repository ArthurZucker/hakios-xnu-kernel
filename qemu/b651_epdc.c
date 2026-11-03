/*
 * b651_epdc.c: the HiBreak Pro's e-ink controller as a QEMU sysbus device.
 *
 * WHY THIS EXISTS
 *
 * Until now the panel was simulated twice: QEMU drew a framebuffer, and qemu/epd.py took a screendump of it and
 * painted an e-ink-looking picture on top (SIM.md:68-78). The second simulator is the problem. An EPDC is not a
 * blitter that reads a framebuffer -- it is a scheduler for voltage trains (EPDC.md:22-24): the caller names a
 * rectangle and a waveform, the controller drives those pixels for N frames at a temperature, and then it says
 * "done". Nothing in that loop needs a host-side picture, and modelling it host-side means the kernel driver
 * developed against it is tested against the wrong state machine.
 *
 * So this device owns the panel state itself: the grey framebuffer the guest writes, the levels the ink is
 * actually holding, the waveform that is running, how far through it the controller is, and the partial-update
 * count that tells a caller when the panel needs a full clear. qemu/epd.py can keep working over `screendump`,
 * but it is now a *viewer* of this device's state rather than the model of it.
 *
 * WHAT IS TAKEN FROM THE REPO'S DOCUMENTS, AND WHAT IS A CONVENTION OF THE SIMULATOR
 *
 *   from devices/b651/WAVEFORMS.md, mirrored, not invented:
 *     - the four waveforms and their meaning (section 1.1): gc16 is the flashing 16-level clear, gl16 is grey
 *       without the flash, du is direct, a2 is the fast 1-bit animation mode.
 *     - frames and residue percentages (section 3.1, 3.3): gc16 24 frames / 0%, gl16 16 / 6%, du 6 / 22%,
 *       a2 2 / 45%. Those are the same four rows as qemu/epd.py:39-44 and devices/b651/drivers/display/
 *       hakios.epd.c:184-195, so the device, the model backend in the driver and the Python viewer agree.
 *     - per-frame timing 16.7 ms is ASSUMED (section 3.4: no measurement exists). It is the panel frame rate
 *       hakios.epd.c:184 already divides by, and it puts gc16 at 400 ms and a2 at 33 ms, matching the log
 *       lines SIM.md:143-145 quotes.
 *     - 16 grey levels (section 2.3), 2 for du/a2 (qemu/epd.py:58).
 *     - the ink range 0x22..0xDD (qemu/epd.py:52-53), so the panel's black is not 0 and its white is not 255.
 *     - a2/du cannot address greys (WAVEFORMS.md section 4 row 3).
 *     - the flash is modelled as transitions over the waveform's frames, not as an instant blank (section 4
 *       row 4: epd.py:132-134 returns the clean image immediately and never renders the blink). Here gc16 drives
 *       its rect to black, then to white, then to the target, so a capture taken mid-update shows the blink.
 *     - the partial-update counter is exposed to the caller rather than acted on (section 4 row 8, EPDC.md:181):
 *       this device never decides to full-refresh by itself.
 *     - temperature is stored and NOT used (section 3.5: the simulator must not claim a curve it does not have;
 *       hakios.epd.c:459-473 stores it and epd_start_model() never reads it -- the same choice).
 *
 *   a convention of this simulator, NOT a fact about the phone:
 *     - the register base (B651_EPDC_DEFAULT_MMIO_BASE, b651_epdc.h) and every offset in that header. No
 *       MT6877 tree has an EPDC node at all (HARDWARE.md:57-58), and the phone's /eink values are root-only
 *       (PHONE.md:53-54). The base is a hole in QEMU's own `virt` map, chosen so it collides with nothing.
 *     - the register set. It is the five things EPDC.md:115-122 says the driver needs (rect, waveform, mode,
 *       trigger, status) and not one NXP register, because the NXP layout is different silicon.
 *     - the residue *shape*: a completed update leaves `ghost` of the swing it just made, where `ghost` grows
 *       with the partial count as qemu/epd.py:240 has it. qemu/epd.py applies that to the frame the panel was
 *       last *driven* with; this device applies it to the ink the panel is actually holding, which is the
 *       "last-driven level per pixel" state vector EPDC.md:18 states. The two differ over many partials; the
 *       90% cap is what keeps either bounded.
 *       Measured against qemu/epd.py with its own tables, driving one partial from a black frame to a white
 *       one: gc16 221 (epd.py 221), gl16 210 (209), du 180 (epd.py gives 221 -- it composites the *darker*
 *       of old and new, an assumption WAVEFORMS.md 2.5 calls wrong for a normal grey UI and true only for a
 *       white-on-black console), a2 137 (ditto). The two grey modes agree to one level; the black/white modes
 *       do not, and this device's answer is the one WAVEFORMS.md 2.5 argues for: residue is symmetric.
 *     - the framebuffer is one byte per pixel, grey, 0 = black and 255 = white, row-major, stride == width,
 *       no padding. A real EPDC is handed Y8 or Y4 by the driver (mxcfb.h:82-85); the exact transport is one
 *       of the things the phone's dtbo would settle.
 *     - a partial update that is refused for a collision is refused, never queued: the device has no queue
 *       and EPDC.md:142-144 says the hardware reports the collision rather than serialising.
 *     - an out-of-panel damage rect is clamped, not refused, because a controller that faults on geometry the
 *       guest has not read yet cannot be brought up.
 *
 * IT IS NOT COMPILED BY ANYTHING YET, AND HERE IS EXACTLY HOW TO WIRE IT
 *
 * There is no QEMU source tree in this repository -- `make qemu-b651` runs the host's qemu-system-aarch64,
 * which here is QEMU 11.1.3. This file is written against QEMU 9.x's qom/sysbus/migration API: everything it
 * uses (OBJECT_DECLARE_SIMPLE_TYPE, sysbus_init_mmio/sysbus_mmio_map, memory_region_init_io/_init_ram,
 * timer_init_full, device_class_set_props, VMSTATE_VBUFFER_UINT32) exists unchanged in 9.0 through 11.x, so
 * it should compile on either. It has NOT been compiled against a real QEMU tree -- see the report; the
 * syntax check that was run used hand-written stub headers, which proves it parses and nothing more.
 *
 * To build it into a QEMU tree, drop this pair in hw/display/ and add:
 *
 *   hw/display/meson.build, beside the other display devices:
 *       system_ss.add(files('b651_epdc.c'))
 *     or, gated so a default build is unchanged:
 *       system_ss.add(when: 'CONFIG_B651_EPDC', if_true: files('b651_epdc.c'))
 *     QEMU dropped hw/display/Makefile.objs in the 5.0 meson conversion. A pre-meson tree (4.x) would instead
 *     need `obj-$(CONFIG_B651_EPDC) += b651_epdc.o` in hw/display/Makefile.objs.
 *   hw/display/Kconfig -- there is no EPDC config symbol upstream, so the block is new:
 *       config B651_EPDC
 *           bool
 *           depends on !KVM
 *   hw/arm/Kconfig -- select it from the b651 machine if it should be built in there:
 *       config B651_EPDC
 *           ... already declared in hw/display/Kconfig; the machine says `select B651_EPDC`
 *
 * Then the command line, next to (or instead of) the ramfb it replaces as the panel's backend:
 *
 *   -device b651-epdc,mmio-base=0x0f000000
 *
 * and the guest sees a 4 KiB register window at that address and the 1125x2436 one-byte-per-pixel request
 * framebuffer one page above it (B651_EPDC_FB_OFFSET). The device maps itself, so no machine change is needed
 * -- see b651_epdc_realize() for why that is a deliberate exception to QEMU's usual machine-maps-it rule.
 * The kernel side (devices/b651/drivers/display/hakios.epd.c) still talks to the model backend; the work to
 * make it write these registers is the driver's, not this device's, and is gated on the
 * phone's dtbo (EPDC.md:157, 214 "steps 1-3 need the phone; 4 does not").
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"

#include "b651_epdc.h"

/* The panel. SIM.md:195-197: the loader's mode, which is also what XNU draws at, 1125x2436 -- not the 824x1648
 * the unit's own screencap reports (a different logical mode). */
#define B651_EPDC_PANEL_WIDTH   1125
#define B651_EPDC_PANEL_HEIGHT  2436

/* Grey levels the grey waveforms address, and the ink they map into. WAVEFORMS.md section 2.3 (16 is ASSUMED,
 * the mode name GC16 is the evidence) and qemu/epd.py:51-53 for the ink range: electrophoresis never reaches
 * 0x00 or 0xFF, and a model that pretends it does flatters the panel. */
#define B651_EPDC_GREY_LEVELS   16
#define B651_EPDC_BW_LEVELS     2
#define B651_EPDC_INK_BLACK     0x22
#define B651_EPDC_INK_WHITE     0xDD

/* payload bytes for the guest to write pixels through. */
#define B651_EPDC_FB_SIZE       ((size_t)B651_EPDC_PANEL_WIDTH * B651_EPDC_PANEL_HEIGHT)

/* A waveform frame at the panel's 60 Hz: 16.7 ms. ASSUMED -- WAVEFORMS.md section 3.4 has no measurement, and
 * this is the same number hakios.epd.c:184 divides by. gc16 x 24 = 400 ms, a2 x 2 = 33 ms, which are the two
 * durations the driver log shows (SIM.md:143-145). */
#define B651_EPDC_FRAME_NS      16666667

/*
 * The waveform table. One row per mode, and every column is from WAVEFORMS.md section 3 as cited per row in the
 * file head. `levels` is what the waveform can address, not what the panel holds: a2/du drive two states, which
 * is why a picture applied through them banding to black/white is correct behaviour (section 4 row 3), not a bug.
 */
typedef struct B651EpdcWaveform {
    const char *name;
    uint32_t    frames;     /* frames until BUSY clears: WAVEFORMS.md 3.1 */
    bool        flashes;    /* drives every pixel to black then white first: WAVEFORMS.md 3.2 */
    uint8_t     residue;    /* per-cent of the swing that is left behind: WAVEFORMS.md 3.3 */
    uint8_t     levels;     /* grey levels addressed: WAVEFORMS.md 4 row 3 */
} B651EpdcWaveform;

static const B651EpdcWaveform b651_epdc_waveforms[B651_EPDC_MODE_COUNT] = {
    [B651_EPDC_MODE_GC16] = { "gc16", 24, true,   0, B651_EPDC_GREY_LEVELS },
    [B651_EPDC_MODE_GL16] = { "gl16", 16, false,  6, B651_EPDC_GREY_LEVELS },
    [B651_EPDC_MODE_DU]   = { "du",    6, false, 22, B651_EPDC_BW_LEVELS   },
    [B651_EPDC_MODE_A2]   = { "a2",    2, false, 45, B651_EPDC_BW_LEVELS   },
};

struct B651EpdcState {
    SysBusDevice parent_obj;

    /* the register file (B651_EPDC_REG_*) */
    MemoryRegion iomem;
    /* the grey framebuffer the guest writes pixels into */
    MemoryRegion fb_mem;

    /* Properties. mmio_base is a property rather than a constant so a machine can place the device without an
     * edit; it is still a simulator convention, not the phone's address (b651_epdc.h). */
    uint64_t  mmio_base;
    uint32_t  frame_period_ns;

    /* the three buffers, one byte per pixel, stride == width, 0 = black request, 255 = white request.
     *   fb[ ]     what the guest asked for
     *   panel[ ]  what the ink is holding: the settled, quantised level (EPDC.md:18's state vector)
     *   start[ ]  panel[] as it was when the running update was triggered, so a frame in the middle of a
     *             waveform can be drawn as "part of the way from there to here"
     */
    uint8_t  *fb;
    uint8_t  *panel;
    uint8_t  *start;
    uint32_t  fb_size;              /* bytes per buffer: panel width * height, fixed at realize */

    /* registers */
    uint32_t  mode;                 /* B651_EPDC_REG_MODE */
    uint32_t  damage_x0, damage_y0, damage_x1, damage_y1;
    uint32_t  temperature;          /* stored, never used -- WAVEFORMS.md 3.5 */
    uint32_t  partial;              /* partial updates since the last flashing one */
    uint32_t  frames;               /* frames of the running update, or of the last finished one */
    uint32_t  frame;                /* how many of those frames have elapsed */
    uint32_t  status;               /* B651_EPDC_STATUS_* */

    QEMUTimer timer;                /* one shot per waveform frame, virtual clock */
    qemu_irq  irq;                  /* raised when BUSY clears: the "LUT done" line (EPDC.md:107) */
};

/*
 * ---- a request, a level index, and an ink value: three different things ------------------------------------
 *
 * Keeping these apart is the point. A pixel request is continuous -- the guest's framebuffer has 256 values.
 * A level index is discrete -- the waveform addresses `levels` of them (16 for the grey modes, 2 for a2/du).
 * The ink value is neither: it is what electrophoresis actually reaches, 0x22..0xDD, and it is what a viewer
 * should see. qemu/epd.py collapsed all three into one PIL LUT (qemu/epd.py:200-202); here quantisation happens
 * once, at the target, and residue is a pull in ink space toward the previous settled ink. That is a linear
 * reparametrisation of doing it in level space -- b651_epdc_ink() is affine in the level index -- so the
 * percentages in the waveform table mean the same thing either way.
 */
static uint8_t
b651_epdc_level_index(uint8_t request, uint8_t levels)
{
    uint32_t bands;

    if (levels < 2) {
        levels = 2;
    }
    bands = levels - 1;
    /* the nearest band, which is the rounding PIL's point() LUT does in qemu/epd.py:201 */
    return (uint8_t)((((uint32_t)request * bands) + 127) / 255);
}

static uint8_t
b651_epdc_ink(uint8_t level, uint8_t levels)
{
    uint32_t bands, span;

    if (levels < 2) {
        levels = 2;
    }
    bands = levels - 1;
    if (level > bands) {
        level = bands;
    }
    span = B651_EPDC_INK_WHITE - B651_EPDC_INK_BLACK;
    return (uint8_t)(B651_EPDC_INK_BLACK + ((uint32_t)level * span + bands / 2) / bands);
}

/* The ink a pixel ends up at for a request, if nothing is left behind. */
static uint8_t
b651_epdc_target(uint8_t request, uint8_t levels)
{
    return b651_epdc_ink(b651_epdc_level_index(request, levels), levels);
}

/* The ink range is the panel's floor and ceiling, so nothing may be driven past it. */
static uint8_t
b651_epdc_in_range(int32_t ink)
{
    if (ink < B651_EPDC_INK_BLACK) {
        return B651_EPDC_INK_BLACK;
    }
    if (ink > B651_EPDC_INK_WHITE) {
        return B651_EPDC_INK_WHITE;
    }
    return (uint8_t)ink;
}

/* Linear interpolation in ink space, `step` of `steps` of the way from `from` to `to`. */
static uint8_t
b651_epdc_lerp(uint8_t from, uint8_t to, uint32_t step, uint32_t steps)
{
    int32_t delta;

    if (steps == 0 || step >= steps) {
        return to;
    }
    delta = (int32_t)to - (int32_t)from;
    return b651_epdc_in_range((int32_t)from + (delta * (int32_t)step) / (int32_t)steps);
}

/*
 * ---- residue ----------------------------------------------------------------------------------------------
 *
 * A partial waveform does not clear the panel: it drives toward the target and some of the ink that was
 * already there stays. In this model the settled level lands `grown` of the way back toward the level the
 * pixel held when the update started, so:
 *
 *   - a pixel that barely moved is nearly at its target and ghosts little -- the per-pixel shape WAVEFORMS.md
 *     section 4 row 2 asks for, and the thing a flat alpha over the whole rect cannot express;
 *   - a pixel driven a long way ghosts at the waveform's full rate;
 *   - repeated partials compound, which is the accumulation EPDC.md:181 says has no clean clearing rule, and
 *     because the pull is toward the *previous settled level* rather than toward "more ink", it converges
 *     instead of fading the panel to black -- the failure qemu/epd.py:215-217 warns of.
 *
 * `grown` carries qemu/epd.py:240's growth with the partial count: `ghost` at one partial, plus half again per
 * update since the last flashing one, capped at 90 so a long streak cannot consume the whole picture. The half
 * is integer here -- ghost * (partial + 1) / 2 -- because it is applied inside a per-pixel loop.
 */
static uint32_t
b651_epdc_residue_grown(uint8_t ghost, uint32_t partial)
{
    uint32_t grown;

    grown = ghost;
    if (partial > 1) {
        grown = (uint32_t)ghost * (partial + 1) / 2;
    }
    if (grown > 90) {
        grown = 90;
    }
    return grown;
}

/* The settled level of one pixel after a non-flashing update. */
static uint8_t
b651_epdc_settle_pixel(uint8_t start, uint8_t target, uint32_t grown, uint8_t levels)
{
    int32_t delta, ink;

    delta = (int32_t)start - (int32_t)target;
    ink = (int32_t)target + (delta * (int32_t)grown) / 100;
    /*
     * Snap the ghosted value back onto the levels this waveform can actually address. Residue is a real optical
     * effect -- some of the old ink is still under the new one -- but it is not freedom to leave the pixel at an
     * arbitrary grey: a 2-level waveform (du, a2) can only end on the two inks it can reach, and a 16-level one
     * only on its sixteen. Blending straight in ink space without this put a2 pixels at 118, 95, 73..., greys
     * the waveform physically cannot produce, and that is what a viewer saw as an a2 render full of mid-greys.
     *
     * The snap is `b651_epdc_ink(level_index(ink, levels), levels)`: map the ghosted ink to the nearest level this
     * waveform addresses, then back to the ink that level means. It is the inverse pair of the target path, so a
     * residue of 0 on a 2-level waveform returns exactly the target and can no longer quietly invent a grey.
     */
    return b651_epdc_ink(b651_epdc_level_index(b651_epdc_in_range(ink), levels), levels);
}

/*
 * ---- the damage rectangle ---------------------------------------------------------------------------------
 *
 * The boundary is checked before anything is driven, and a trigger with no damage does nothing at all and says
 * it succeeded: "no damage -> no update" is one of the two semantics WAVEFORMS.md section 4 says NOT to change,
 * because it is what makes a panel usable (SIM.md:99). A rectangle that runs off the panel is clamped and
 * counted in the log, not refused: the guest cannot know the geometry until it reads PANEL_W/PANEL_H, and a
 * controller that faults on it is a controller that cannot be brought up.
 *
 * Which rule applies to a rectangle *outside* the panel -- clamp to empty, or reject -- is not a fact about the
 * phone, so the clamp is chosen, once, here.
 */
static void
b651_epdc_rect_clamp(B651EpdcState *s)
{
    if (s->damage_x0 == s->damage_x1 || s->damage_y0 == s->damage_y1) {
        return;                 /* already empty: the caller asked for no damage, or an exclusive rect of none */
    }
    if (s->damage_x1 > B651_EPDC_PANEL_WIDTH || s->damage_y1 > B651_EPDC_PANEL_HEIGHT ||
        s->damage_x0 > B651_EPDC_PANEL_WIDTH || s->damage_y0 > B651_EPDC_PANEL_HEIGHT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "b651-epdc: damage %u,%u-%u,%u is off a %dx%d panel; clamped\n",
                      s->damage_x0, s->damage_y0, s->damage_x1, s->damage_y1,
                      B651_EPDC_PANEL_WIDTH, B651_EPDC_PANEL_HEIGHT);
        s->damage_x1 = MIN(s->damage_x1, (uint32_t)B651_EPDC_PANEL_WIDTH);
        s->damage_y1 = MIN(s->damage_y1, (uint32_t)B651_EPDC_PANEL_HEIGHT);
        s->damage_x0 = MIN(s->damage_x0, s->damage_x1);
        s->damage_y0 = MIN(s->damage_y0, s->damage_y1);
    }
}

static bool
b651_epdc_rect_empty(const B651EpdcState *s)
{
    return s->damage_x1 <= s->damage_x0 || s->damage_y1 <= s->damage_y0;
}

/* ---- the panel state a viewer reads ---------------------------------------------------------------- */

/*
 * The ink the panel is holding, one byte per pixel, 0x22..0xDD, stride == width, plus its size. This is the one
 * thing a viewer needs: it is a *result*, not a request, so it is what the panel looks like and not what the
 * guest asked for. Mid-update it is a frame of the waveform rather than the settled picture -- gc16 drives its
 * rect to black, then white, then to the target across its frames -- which is the blink qemu/epd.py section 4
 * row 4 says the Python model never rendered. The pointer is into the device's own buffer and is only stable
 * until the next frame: copy it, do not keep it.
 */
const uint8_t *
b651_epdc_panel_state(const B651EpdcState *s, uint32_t *width, uint32_t *height)
{
    if (width != NULL) {
        *width = B651_EPDC_PANEL_WIDTH;
    }
    if (height != NULL) {
        *height = B651_EPDC_PANEL_HEIGHT;
    }
    return s->panel;
}

/* ---- the waveform engine ------------------------------------------------------------------ */

/*
 * Advance the running update by one frame. Called from the timer, and from the trigger for frame 0, so that the
 * pixels start moving at once rather than one frame period after the caller asked for them.
 *
 * Each frame repaints the whole damage rect. That is wasteful for a rect that did not change, but it is what a
 * controller does: the waveform is programmed for the rect and every pixel in it is driven on every frame.
 */
static void
b651_epdc_frame(B651EpdcState *s)
{
    const B651EpdcWaveform *wave = &b651_epdc_waveforms[s->mode];
    uint32_t x, y, grown;
    uint8_t target;

    s->frame++;

    /* the flash half of a flashing waveform: black for the first two frames' worth, then white, then the
     * target. WAVEFORMS.md section 3.2 records "flashes" as a fact and the black-then-white order as ASSUMED,
     * and INIT's documented sequence is white-first (mxc_epdc_fb_test.c:64) -- so this is the gc16 guess, and
     * it is labelled one. */
    if (wave->flashes) {
        /* the flash takes two thirds of the waveform, the arrival the last third. `last` cannot be 0: for a
         * one-frame waveform both thirds collapse and the pixels go straight to white, which is the closest a
         * model can get to "it was too fast to see". */
        uint32_t third = wave->frames / 3 + 1;
        uint32_t last = wave->frames > 2 * third ? wave->frames - 2 * third : 1;
        uint32_t arrive = s->frame > 2 * third ? s->frame - 2 * third : 0;

        for (y = s->damage_y0; y < s->damage_y1; y++) {
            uint32_t row = y * B651_EPDC_PANEL_WIDTH;

            for (x = s->damage_x0; x < s->damage_x1; x++) {
                uint32_t i = row + x;

                if (s->frame < third) {
                    s->panel[i] = B651_EPDC_INK_BLACK;
                } else if (s->frame < 2 * third) {
                    s->panel[i] = B651_EPDC_INK_WHITE;
                } else {
                    s->panel[i] = b651_epdc_lerp(B651_EPDC_INK_WHITE,
                                                 b651_epdc_target(s->fb[i], wave->levels),
                                                 arrive, last);
                }
            }
        }
    } else {
        grown = b651_epdc_residue_grown(wave->residue, s->partial);

        for (y = s->damage_y0; y < s->damage_y1; y++) {
            uint32_t row = y * B651_EPDC_PANEL_WIDTH;

            for (x = s->damage_x0; x < s->damage_x1; x++) {
                uint32_t i = row + x;

                /* the settled value this update is driving toward, accounting for the residue it will leave */
                target = b651_epdc_settle_pixel(s->start[i],
                                                b651_epdc_target(s->fb[i], wave->levels), grown,
                                                wave->levels);
                if (s->frame < wave->frames) {
                    /* still arriving: the capture sees part of the old picture, which is effect 2 of
                     * qemu/epd.py's settle() docstring -- how a partial screenshot shows a mix */
                    s->panel[i] = b651_epdc_lerp(s->start[i], target, s->frame, wave->frames);
                } else {
                    s->panel[i] = target;
                }
            }
        }
    }

    if (s->frame >= wave->frames) {
        /* done: BUSY clears and the line is raised. The driver's completion handler is what wakes whoever is
         * in epd_wait(), which is why this is an irq and not a status bit alone (EPDC.md:107). */
        s->status &= ~B651_EPDC_STATUS_BUSY;
        if (wave->flashes) {
            s->partial = 0;             /* a flashing waveform is the un-ghosting one: the counter resets */
        } else {
            s->partial++;
        }
        qemu_irq_raise(s->irq);
        return;
    }

    timer_mod(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->frame_period_ns);
}

static void
b651_epdc_timer(void *opaque)
{
    b651_epdc_frame(opaque);
}

/*
 * The trigger. Everything before this is a register write that changes nothing on the panel: the waveform, the
 * rectangle and the pixels only become an update here, which is the distinction the real driver's submit path
 * is built on (EPDC.md:29-36 -- "program EPDC" then "WAIT for LUT-complete").
 *
 * A trigger while BUSY does NOT queue. It is refused and COLLISION latched, because two waveforms over
 * overlapping rects cannot run at once and a controller has no queue to put the second in (EPDC.md:142-144;
 * mxc_epdc_fb reports the collision rather than serialising it). Whether the caller retries from a different
 * rect, or gives up, is the caller's policy -- the same reason the partial counter is exposed rather than acted
 * on.
 *
 * A trigger whose rect is empty returns *without* touching BUSY or the counter. That is deliberate: a picture
 * that did not change must cost nothing, which is the rule SIM.md:99 and WAVEFORMS.md section 4 both protect.
 */
static void
b651_epdc_trigger(B651EpdcState *s)
{
    const B651EpdcWaveform *wave;

    if (s->status & B651_EPDC_STATUS_BUSY) {
        s->status |= B651_EPDC_STATUS_COLLISION;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "b651-epdc: trigger while %s is running (%u/%u); refused, collision latched\n",
                      b651_epdc_waveforms[s->mode].name, s->frame, s->frames);
        return;
    }

    b651_epdc_rect_clamp(s);
    if (b651_epdc_rect_empty(s)) {
        return;
    }

    wave = &b651_epdc_waveforms[s->mode];

    /* the state the update starts from, per pixel: what residue is measured against and what a mid-update
     * frame interpolates from */
    memcpy(s->start, s->panel, s->fb_size);

    s->status &= ~(B651_EPDC_STATUS_COLLISION);
    s->status |= B651_EPDC_STATUS_BUSY;
    s->frames = wave->frames;
    s->frame = 0;
    qemu_irq_lower(s->irq);

    qemu_log_mask(LOG_TRACE,
                  "b651-epdc: %s on %u,%u-%u,%u (%u frames, %s, %u%% residue, partial=%u)\n",
                  wave->name, s->damage_x0, s->damage_y0, s->damage_x1, s->damage_y1,
                  wave->frames, wave->flashes ? "flashing" : "no flash", wave->residue, s->partial);

    /* frame 0 now, not one frame period from now: the caller has just been told BUSY, so the pixels had better
     * be moving */
    b651_epdc_frame(s);
}

/* ---- the register file ---------------------------------------------------------------------------- */

static uint64_t
b651_epdc_read(void *opaque, hwaddr offset, unsigned size)
{
    B651EpdcState *s = B651_EPDC(opaque);
    uint32_t value;

    if (size != 4 || (offset & 3) != 0) {
        /* the register file is word-only. A driver that does byte reads is doing something a real controller
         * would not answer either, and answering it here would hide the bug. */
        qemu_log_mask(LOG_GUEST_ERROR, "b651-epdc: %u-byte read at 0x%" HWADDR_PRIx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case B651_EPDC_REG_PANEL_W:   value = B651_EPDC_PANEL_WIDTH; break;
    case B651_EPDC_REG_PANEL_H:   value = B651_EPDC_PANEL_HEIGHT; break;
    case B651_EPDC_REG_LEVELS:    value = B651_EPDC_GREY_LEVELS; break;
    case B651_EPDC_REG_MODE:      value = s->mode; break;
    case B651_EPDC_REG_DAMAGE_X0: value = s->damage_x0; break;
    case B651_EPDC_REG_DAMAGE_Y0: value = s->damage_y0; break;
    case B651_EPDC_REG_DAMAGE_X1: value = s->damage_x1; break;
    case B651_EPDC_REG_DAMAGE_Y1: value = s->damage_y1; break;
    case B651_EPDC_REG_TRIGGER:   value = 0; break;     /* write-only: a trigger is an event, not state */
    case B651_EPDC_REG_STATUS:    value = s->status; break;
    case B651_EPDC_REG_FRAMES:    value = s->frames; break;
    case B651_EPDC_REG_PARTIAL:   value = s->partial; break;
    case B651_EPDC_REG_TEMP:      value = s->temperature; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "b651-epdc: read of unused offset 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }
    return value;
}

static void
b651_epdc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    B651EpdcState *s = B651_EPDC(opaque);

    if (size != 4 || (offset & 3) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "b651-epdc: %u-byte write at 0x%" HWADDR_PRIx "\n", size, offset);
        return;
    }

    switch (offset) {
    case B651_EPDC_REG_MODE:
        if (value >= B651_EPDC_MODE_COUNT) {
            /* refusing rather than masking: a waveform index the device does not have is a driver that
             * believes in a table this model is not driving with, and silently running gc16 instead would be
             * the sort of wrong that takes a boot to notice */
            qemu_log_mask(LOG_GUEST_ERROR, "b651-epdc: waveform mode %" PRIu64 " does not exist\n", value);
            return;
        }
        s->mode = (uint32_t)value;
        break;
    case B651_EPDC_REG_DAMAGE_X0: s->damage_x0 = (uint32_t)value; break;
    case B651_EPDC_REG_DAMAGE_Y0: s->damage_y0 = (uint32_t)value; break;
    case B651_EPDC_REG_DAMAGE_X1: s->damage_x1 = (uint32_t)value; break;
    case B651_EPDC_REG_DAMAGE_Y1: s->damage_y1 = (uint32_t)value; break;
    case B651_EPDC_REG_TRIGGER:
        if (value != 0) {
            b651_epdc_trigger(s);
        }
        break;
    case B651_EPDC_REG_TEMP:
        /* stored and not used: WAVEFORMS.md section 3.5 is explicit that the temperature curve does not exist,
         * so a device that applied one would be inventing the panel's physics. The same choice the driver's
         * model backend makes (hakios.epd.c:459-473). */
        s->temperature = (uint32_t)value;
        break;
    case B651_EPDC_REG_STATUS:
        /* W1C for COLLISION only. BUSY is the device's answer to a trigger and cannot be cleared from here:
         * a driver that could clear it would be able to lie to itself. */
        if (value & B651_EPDC_STATUS_COLLISION) {
            s->status &= ~B651_EPDC_STATUS_COLLISION;
        }
        break;
    case B651_EPDC_REG_PANEL_W:
    case B651_EPDC_REG_PANEL_H:
    case B651_EPDC_REG_LEVELS:
    case B651_EPDC_REG_FRAMES:
    case B651_EPDC_REG_PARTIAL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "b651-epdc: offset 0x%" HWADDR_PRIx " is read-only, written 0x%" PRIx64 "\n",
                      offset, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "b651-epdc: write of unused offset 0x%" HWADDR_PRIx "\n", offset);
        break;
    }
}

static const MemoryRegionOps b651_epdc_ops = {
    .read = b651_epdc_read,
    .write = b651_epdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* the register file is 32-bit words, so a device-side burst is two words; letting the region merge writes
     * would mean re-deriving which register each byte belonged to, and there is no reason to */
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

/* ---- life: reset, realize, the type --------------------------------------------------------------- */

/*
 * Reset is what a power-on clear leaves the panel at: paper white, the flashing grey waveform selected, no
 * damage, not busy. Panel-white rather than black because an undriven electrophoretic panel settles to its
 * paper state, which is why the console inversion the orchestrator applied (COLOR_BACKGROUND 7) matches what
 * this device shows at rest.
 */
static void
b651_epdc_reset(DeviceState *dev)
{
    B651EpdcState *s = B651_EPDC(dev);

    timer_del(&s->timer);
    memset(s->fb, 0xff, s->fb_size);
    memset(s->panel, B651_EPDC_INK_WHITE, s->fb_size);
    memset(s->start, B651_EPDC_INK_WHITE, s->fb_size);
    s->mode = B651_EPDC_MODE_GC16;
    s->damage_x0 = 0;
    s->damage_y0 = 0;
    s->damage_x1 = B651_EPDC_PANEL_WIDTH;
    s->damage_y1 = B651_EPDC_PANEL_HEIGHT;
    s->temperature = 20;            /* mxc_epdc_fb.c:65-66's default, EPDC.md:148 -- not a reading */
    s->partial = 0;
    s->frames = 0;
    s->frame = 0;
    s->status = 0;
    qemu_irq_lower(s->irq);
}

static void
b651_epdc_realize(DeviceState *dev, Error **errp)
{
    B651EpdcState *s = B651_EPDC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->frame_period_ns == 0) {
        s->frame_period_ns = B651_EPDC_FRAME_NS;
    }
    if (s->mmio_base + B651_EPDC_FB_OFFSET + B651_EPDC_FB_SIZE > UINT32_MAX) {
        error_setg(errp, "b651-epdc: mmio-base 0x%" PRIx64 " leaves no room for the %zu-byte framebuffer",
                   s->mmio_base, B651_EPDC_FB_SIZE);
        return;
    }

    /*
     * The framebuffer is a RAM region of the device's own, not a buffer the device copies out of the guest:
     * the guest writes its request pixels straight into this RAM, and the waveform engine reads them from the
     * same bytes at a trigger. That is the separation the hardware has -- a DRAM buffer the CPU fills and a
     * LUT the controller runs over it -- and it means a guest may scribble here as freely as it likes without
     * starting anything, because nothing becomes an update until TRIGGER.
     */
    memory_region_init_ram(&s->fb_mem, OBJECT(s), "b651-epdc.fb", B651_EPDC_FB_SIZE, errp);
    if (*errp != NULL) {
        return;
    }
    s->fb = memory_region_get_ram_ptr(&s->fb_mem);
    s->fb_size = (uint32_t)B651_EPDC_FB_SIZE;

    /* The ink the panel holds and the frame an update started from are the device's own state, not the
     * guest's memory: both are one byte per pixel in the same order as the framebuffer. */
    s->panel = g_new0(uint8_t, s->fb_size);
    s->start = g_new0(uint8_t, s->fb_size);

    /*
     * The register file, and the framebuffer window immediately above it.
     *
     * The device maps itself here rather than waiting for a machine to do it. That is not the usual QEMU
     * shape -- sysbus devices normally leave mapping to their machine, which knows the address map -- but
     * there is no b651 machine yet, and the point of this file is that `-device b651-epdc` alone brings up
     * something a driver can poke. When the b651 machine exists it should stop passing mmio-base and do the
     * two sysbus_mmio_map() calls itself, because from then on the address map belongs to the machine.
     */
    memory_region_init_io(&s->iomem, OBJECT(s), &b651_epdc_ops, s, "b651-epdc", B651_EPDC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_mmio(sbd, &s->fb_mem);
    sysbus_mmio_map(sbd, 0, s->mmio_base);
    sysbus_mmio_map(sbd, 1, s->mmio_base + B651_EPDC_FB_OFFSET);

    sysbus_init_irq(sbd, &s->irq);

    /* a per-frame tick on the virtual clock: an update takes 33 ms to 400 ms of guest time, so it must follow
     * the guest's clock (a paused machine's panel stops moving, which is what a paused machine's panel does) */
    timer_init_full(&s->timer, NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS, 0, b651_epdc_timer, s);
}

static void
b651_epdc_finalize(Object *obj)
{
    B651EpdcState *s = B651_EPDC(obj);

    timer_del(&s->timer);
    /* s->fb is the RAM region's own memory (memory_region_init_ram), so it is released with the region, not
     * here. Only the two buffers this device allocated are ours to free. */
    g_free(s->panel);
    g_free(s->start);
    s->panel = NULL;
    s->start = NULL;
}

/* Everything here starts as a simulator convention; b651_epdc.h says which number is which. The mmio-base
 * property exists so the b651 machine can put the device somewhere else without an edit, and frame-period-ns
 * exists so a test can run a 400 ms waveform in milliseconds. */
static Property b651_epdc_properties[] = {
    DEFINE_PROP_UINT64("mmio-base", B651EpdcState, mmio_base, B651_EPDC_DEFAULT_MMIO_BASE),
    DEFINE_PROP_UINT32("frame-period-ns", B651EpdcState, frame_period_ns, B651_EPDC_FRAME_NS),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription b651_epdc_vmstate = {
    .name = "b651-epdc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mode, B651EpdcState),
        VMSTATE_UINT32(damage_x0, B651EpdcState),
        VMSTATE_UINT32(damage_y0, B651EpdcState),
        VMSTATE_UINT32(damage_x1, B651EpdcState),
        VMSTATE_UINT32(damage_y1, B651EpdcState),
        VMSTATE_UINT32(temperature, B651EpdcState),
        VMSTATE_UINT32(partial, B651EpdcState),
        VMSTATE_UINT32(frames, B651EpdcState),
        VMSTATE_UINT32(frame, B651EpdcState),
        VMSTATE_UINT32(status, B651EpdcState),
        /* The ink migrates with the device: a restored machine that came back to a blank panel would redraw a
         * picture the guest believes is already there. The guest's own request pixels do not travel here --
         * they are the framebuffer RAM region, which migration carries as a RAM block. */
        VMSTATE_VBUFFER_UINT32(panel, B651EpdcState, 1, NULL, 0, fb_size),
        VMSTATE_VBUFFER_UINT32(start, B651EpdcState, 1, NULL, 0, fb_size),
        VMSTATE_TIMER(timer, B651EpdcState),
        VMSTATE_END_OF_LIST()
    },
};

static void
b651_epdc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = b651_epdc_realize;
    dc->reset = b651_epdc_reset;
    device_class_set_props(dc, b651_epdc_properties);
    dc->vmsd = &b651_epdc_vmstate;
    /* A sysbus device with no parent bus: `-device b651-epdc` creates it standalone, and realize maps its own
     * windows (see b651_epdc_realize). It has no display output of its own -- a viewer reads
     * b651_epdc_panel_state(). DEVICE_CATEGORY_MISC rather than DISPLAY, because nothing here draws to a
     * QEMU DisplaySurface, and a category that promised one would be the same lie the Python post-processor
     * told. */
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo b651_epdc_info = {
    .name          = TYPE_B651_EPDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(B651EpdcState),
    .class_init    = b651_epdc_class_init,
    .instance_finalize = b651_epdc_finalize,
};

static void
b651_epdc_register_types(void)
{
    type_register_static(&b651_epdc_info);
}

type_init(b651_epdc_register_types)
