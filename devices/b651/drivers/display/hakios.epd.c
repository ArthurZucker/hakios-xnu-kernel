/*
 * /dev/hakios-epd: the e-ink panel's controller (EPDC) of the Bigme HiBreak Pro (b651), as a character
 * device. Only with boot-arg epd=1. This file is a SKELETON: it defines the device and the four commands
 * the port needs, and has no working path to the panel yet.
 *
 * The design is devices/b651/EPDC.md, section 2.2. This is not an IOFramebuffer, and must not become one
 * yet (EPDC.md:68-92): an e-ink panel does not hold a frame for a scanout engine to read, and it does not
 * finish an update in 16.7 ms. What it has is a request -- "this rect, this waveform, tell me when it is
 * done" -- and a waveform LUT that drives each pixel for 20-900 ms at a temperature (EPDC.md:8-38).
 *
 * WHERE EVERY NUMBER BELOW COMES FROM, AND WHAT IS NOT KNOWN
 *
 *   the ABI           ours, from EPDC.md:115-127. struct hakios_epd_update is mxcfb_update_data minus the
 *                     alt-buffer and PxP fields (include/uapi/linux/mxcfb.h:131-141 in nxp-imx/linux-imx,
 *                     read as the design -- no code is copied; see EPDC.md:183 on the licence). The two
 *                     update modes are that file's UPDATE_MODE_PARTIAL 0 and UPDATE_MODE_FULL 1. The four
 *                     commands are the four EPDC.md:117-122 says the set must be, and no more.
 *   the ioctl numbers ours, group 'E'. 'E' is not used by any other ioctl of the kernel this is built
 *                     against (XNU 8796.121.2; kern_event.h uses 'e', lower case, 1-4). The numbers are
 *                     computed by <sys/ioccom.h> from the structs, not written as hex.
 *   the default 20 C  mxc_epdc_fb.c:65-66's default, not a reading of this panel (EPDC.md:148,182).
 *   the device name   "hakios-epd" (EPDC.md:70). Major -1 (any), UID_ROOT/GID_WHEEL, 0666: the house's
 *                     own character devices (drivers/input/hakios.input.c:224).
 *
 *   NOT KNOWN, so no value for it is written anywhere in this file. Each is a 0 in `epd_hw` below, and
 *   epd_attach() says which one stopped it:
 *     - the controller's identity and its register base and size. UNVERIFIED. No MT6877 source has an
 *       EPDC node at all (HARDWARE.md:57-58); the phone's OWN device tree does have /eink and
 *       /epd_pmic_det, but their property values are root-only and the phone's dtbo has not been read
 *       (PHONE.md:50-66, 136-143). EPDC.md:157 is the same statement.
 *     - its register layout, its interrupt number and its maximum update width. Same source; also
 *       EPDC.md:47 for the width, which decides whether an update must be split.
 *     - the panel's geometry as the CONTROLLER states it: /eink names eink_fb_width, eink_fb_height, eink_width
 *       and eink_height, and their values are not readable without root (PHONE.md:58,138-139). A rect is checked
 *       against the PANEL anyway, from the boot framebuffer's own size -- see epd_check() for why that one is
 *       readable without root, and for the one thing it still cannot check (the controller's burst width).
 *     - the waveform LUTs and their temperature range table: the `waveform` partition and mblock-20-waveform
 *       (PHONE.md:41-42,73-77). The format is unverified, and a wrong LUT can damage the panel
 *       (EPDC.md:177). Nothing here selects or uploads one.
 *     - the power sequence, VCOM and the EPD PMIC. The board carries either a SYV776A on I2C (address
 *       0x62, from Android's own side: EPDC.md:54,161) or a tps65185; which this unit has is a root-level
 *       question (PHONE.md:83-84). VCOM is per unit, and EPDC.md:178 is explicit: never write one that
 *       was not first read. Nothing here writes a PMIC, a rail or a GPIO.
 *
 * WHAT EACH COMMAND RETURNS. The ioctls are group 'E'; <sys/ioccom.h> makes the numbers from these:
 *
 *   HAKIOS_EPD_UPDATE     _IOW('E', 1, struct hakios_epd_update)
 *       0        the update was accepted: with the model backend it is now driving for `frames * 16.6` ms and
 *                HAKIOS_EPD_WAIT will return when it is over. On the phone -- epd=1 with no epd_model=1 -- nothing
 *                reaches this answer, because epd_attach() has said ENOTSUP and epd_attached is false
 *       ENOTSUP  there is no controller to drive: epd_attach() found no EPDC and no model backend to stand for one
 *       EINVAL   a rect with no area, a rect whose right or bottom edge runs past the panel, a coordinate that
 *                runs past 2^32, marker 0, an update_mode that is neither PARTIAL nor FULL, or a waveform_mode
 *                this driver has no model for. All decided before the controller is looked at, so that a
 *                caller's own mistake is still named when there is no panel to drive
 *       EBUSY    an update is already in flight (one at a time: this backend can stand for one LUT, not several).
 *                The marker in flight is set before the update starts, not after, so a caller cannot get 0 and
 *                then a second 0 while the first is still settling
 *   HAKIOS_EPD_WAIT       _IOWR('E', 2, struct hakios_epd_wait)
 *       0        the update at that marker has finished; `collision` says which LUTs were in the way. A marker
 *                that is not in flight returns 0 at once, which is right: there is nothing to wait for. Only a
 *                marker that IS in flight sleeps, so "0" after an update was accepted does mean the ink settled
 *       ENOTSUP  there is no controller and no model, so no update can have been accepted
 *       EINTR    a signal arrived while waiting (msleep's PCATCH: ERESTART too, for a signal that
 *                does not interrupt; /dev/hakios-touch's read passes both on in the same way)
 *   HAKIOS_EPD_SET_TEMP   _IOW('E', 3, int32_t)
 *       0        stored, for the waveform select that does not exist yet
 *       EINVAL   HAKIOS_EPD_TEMP_UNSET came in as a temperature
 *   HAKIOS_EPD_POWERDOWN  _IOW('E', 4, int32_t)
 *       0        the delay was recorded (ms; -1 = never); with 0, the panel was powered down if nothing was in
 *                flight. With the model backend that is a call into epd_powerdown(), which writes nothing until
 *                the PMIC sequence is known, so "powered down" is a promise the skeleton cannot keep yet either
 *       EINVAL   a delay below -1
 *       EBUSY    the delay was recorded, but 0 came in with an update still in flight and the powerdown was
 *                refused (EPDC.md:132,145: idle means no LUT active and no pending update). Recorded first, refused
 *                after: the setting stands, the act does not
 *   anything else                                                        ENOTTY
 *
 *   write() takes a stream of struct hakios_epd_update, each one handled as HAKIOS_EPD_UPDATE is, and
 *   stops at the first refused record (the same shape as /dev/hakios-touch's writes). A stream that is not a
 *   whole number of records is EINVAL and nothing is submitted; the whole records of such a stream are not
 *   quietly consumed either, so a caller that miscounted hears about it instead of half-drawing a screen.
 *   read() is not implemented (eno_rdwrt): the answers are the ioctls'.
 *
 * WHAT IS DELIBERATELY ABSENT (EPDC.md:124-127): waveform-mode upload (MXCFB_SET_WAVEFORM_MODES),
 *   auto-update mode, update scheme, dithering, alt-buffer, GET_WORK_BUFFER. Each is a caller-side feature
 *   laid over the four commands above; adding one before a full refresh works is how this gate never
 *   closes. epd_complete() -- the LUT-done handler of EPDC.md:107 -- is no longer one of the absent things: the
 *   model backend gives it a caller (model_done(), the model's timer), and it clears the marker in flight and wakes
 *   the waiters of HAKIOS_EPD_WAIT exactly as it will on the phone. What is still absent is the INTERRUPT that will
 *   call it there: the controller's IRQ is one of the unknowns above, and an interrupt handler with no vector to
 *   attach it to would be dead code -- which is what the whole of what it does was, before the simulator.
 *
 * Status: RUNS IN THE SIMULATOR. `epd=1 epd_model=1` on qemu-b651 attaches the model backend: an update drives for
 *   the waveform's time and then completes, so the ABI, the marker in flight, EBUSY, epd_wait()'s sleep and wake,
 *   the collision bitmap and the update/powerdown ordering are all exercised on a machine that cannot be hurt.
 *   What is NOT exercised is everything below that: no register, no LUT, no interrupt, no PMIC, no VCOM.
 *   It IS a skeleton in one sense still worth saying: it is the model, not the panel. Nothing below the model
 *   exists, and the model's durations are epd.py's, not this panel's (WAVEFORMS.md:163-166).
 *   Not run on the phone. `epd=1` alone there still reaches epd_attach()'s ENOTSUP, because epd_model=1 is only in
 *   the simulator's payload cmdline. What comes next, in order, is EPDC.md:200-214: the phone's dtbo (/eink,
 *   /epd_pmic_det) for the controller, and the `waveform` and `vcom` partitions for the LUTs and the per-unit VCOM
 *   -- then `epd_hw` below is filled in and the register backend is written beside this one.
 */
#include <stdint.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <console/video_console.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>

/* The boot framebuffer: the panel's own geometry, which this driver bounds an update against (see epd_check()).
 * Declared the way the two display drivers beside this one declare it (hakios.fb.c:42, hakios.virtio_gpu.c:52). */
extern struct vc_info vinfo;

/* ---- the ABI: what user space sends and asks ------------------------------------------------------------
 * This lives here because a device's repository may add no file to xnu/files/; when a caller is written
 * (the text console, or MiniIOMobileFramebuffer's drawRows, EPDC.md:78-92) it moves to
 * sys/hakios.epd.h, as /dev/hakios-touch's record did (sys/hakios.input.h).
 */
#define HAKIOS_EPD_TEMP_UNSET   INT32_MIN       /* ours: "no temperature in this request" (not a reading) */
#define HAKIOS_EPD_TEMP_DEFAULT 20              /* C; mxc_epdc_fb.c:65-66, EPDC.md:148 -- not measured here */

#define HAKIOS_EPD_UPDATE_PARTIAL 0             /* mxcfb.h UPDATE_MODE_PARTIAL */
#define HAKIOS_EPD_UPDATE_FULL    1             /* mxcfb.h UPDATE_MODE_FULL (0x1) */

struct hakios_epd_update {
	uint32_t top;                   /* pixels from the panel's top edge (mxcfb's (0,0) is top left) */
	uint32_t left;                  /* pixels from its left edge */
	uint32_t width;                 /* pixels; the panel's own bounds are not known yet, see above */
	uint32_t height;
	uint32_t update_mode;           /* HAKIOS_EPD_UPDATE_PARTIAL or _FULL */
	uint32_t waveform_mode;         /* opaque here, 0 = the blob's default. Android's integers (4, 132, 1029,
	                                 * 178, 180: EPDC.md:55,162) have no LUT to point at yet */
	uint32_t marker;                /* the caller's, must not be 0; what HAKIOS_EPD_WAIT waits on */
	int32_t  temperature;           /* C; HAKIOS_EPD_TEMP_UNSET = the one HAKIOS_EPD_SET_TEMP set */
};

struct hakios_epd_wait {
	uint32_t marker;                /* in: the update to wait for; 0 = every update submitted */
	uint32_t collision;             /* out: one bit per LUT that had already started on an overlapping
	                                 * rect, so the caller knows to redraw (mxcfb's collision bitmap,
	                                 * EPDC.md:120); always 0 until there are LUTs */
};

/*
 * WHAT THE DAMAGE RECT IS, AND THE SHAPE THE GPU DRIVER ALREADY USES. The virtio GPU driver publishes a damage rect
 * on /dev/hakios-gpu: the union of the rects it transferred since somebody last read it, as struct hakios_gpu_damage
 * (drivers/gpu/hakios.virtio_gpu.c:54-79). Its first four fields are `top, left, width, height`, in pixels from the
 * panel's top-left corner -- and that is this file's `struct hakios_epd_update`'s own first four fields, in its own
 * order (hakios.virtio_gpu.c:57-60 says so, and names this file as the one it follows). So a rect read from the GPU
 * device can be copied into an update with no translation: this driver's rect is ALREADY in the shape the GPU driver
 * produces, and there is no second shape to convert to. That is also why there is no damage ioctl here -- EPDC.md:115
 * says the four-command set must not grow, and the rect a caller needs to drive is the rect it just submitted.
 *
 * Two differences that are real and are not about shape:
 *   - the GPU's rect is a full-width vertical SPAN of rows, because it compares 64-row bands and takes the span of
 *     the dirty ones (hakios.virtio_gpu.c:267-273, :279). An update here carries a tight box; a span is simply a
 *     legal box with left 0 and width = the panel's.
 *   - the GPU accumulates its rect and clears it on read (:284-293). This driver takes one update at a time and keeps
 *     no history, so there is nothing to accumulate: the marker in flight is the whole of its state.
 */

#define HAKIOS_EPD_UPDATE       ((u_long)_IOW('E', 1, struct hakios_epd_update))
#define HAKIOS_EPD_WAIT         _IOWR('E', 2, struct hakios_epd_wait)
#define HAKIOS_EPD_SET_TEMP     ((u_long)_IOW('E', 3, int32_t))
#define HAKIOS_EPD_POWERDOWN    ((u_long)_IOW('E', 4, int32_t))

/* ---- the facts this driver needs, and where they will come from -----------------------------------------
 * Every field is 0 until it is known. Nothing here is guessed: a 0 is "not known", and epd_attach() names
 * the first one that is. The source of each, when it is read, is written beside it.
 * One block, as drivers/bluetooth/hakios.bt.c keeps the pins it looks at (`pins`).
 */
static struct hakios_epd_hw {
	vm_offset_t base;               /* 0: UNVERIFIED. The EPDC's register base. Source: the phone's dtbo,
	                                 * its /eink node (PHONE.md:50-66); no MT6877 Linux source has one
	                                 * (HARDWARE.md:57-58) */
	vm_size_t size;                 /* 0: UNVERIFIED. Its window. Same source */
	uint32_t irq;                   /* 0: UNVERIFIED. Its interrupt; the one epd_complete() will need */
	uint32_t max_update_width;      /* 0: UNVERIFIED. Past it an update must be split (EPDC.md:47,160) */
	uint32_t panel_width;           /* 0: UNVERIFIED. /eink's eink_fb_width, root-only (PHONE.md:58,138) */
	uint32_t panel_height;          /* 0: UNVERIFIED. /eink's eink_fb_height, likewise */
} epd_hw;

static LCK_GRP_DECLARE(epd_lck_grp, "hakios-epd");
static LCK_MTX_DECLARE(epd_lock, &epd_lck_grp);

static bool epd_attached;                       /* the controller answered; false in this skeleton */
static uint32_t epd_marker_in_flight;           /* the one marker an update is running under, 0 = none */
static int32_t epd_temperature = HAKIOS_EPD_TEMP_DEFAULT;
static int32_t epd_powerdown_ms = -1;           /* -1 = never (EPDC.md:122). The vendor's own default is
                                                 * in mxc_epdc_fb.c:3199-3218, which has not been read */

/*
 * The simulator's panel. QEMU's virt machine has no EPDC, but it does have the picture on its screen, and the
 * boot framebuffer XNU draws into is that same memory (MiniIOMobileFramebuffer.cpp takes fBase from vinfo).
 * So on QEMU an update is not a register write: it is a timed settle. The rect drives for as many milliseconds
 * as the waveform's model says, one update at a time, and then epd_complete() runs -- through a thread_call,
 * exactly as the controller's interrupt would run it on the phone.
 *
 * The point of this backend is NOT to imitate the panel. It is that everything above it -- epd_check(), the
 * marker in flight, EBUSY, epd_wait()'s sleep and wake, the collision bitmap, the update/powerdown ordering -- is
 * the code the phone will run, with one backend swapped for the other. QEMU has no controller to damage, which
 * is the whole reason to develop the state machine here.
 *
 * The durations are epd.py's MODEL, not measurements of this panel (SIM.md: "it is a model, not the controller":
 * gc16 24 frames, gl16 16, du 6, a2 2 at 60 Hz of panel -- which is why the fast modes are the fast ones).
 */
#define EPD_MODEL_FRAME_MS      1000 / 60               /* a waveform frame at the panel's 60 Hz. UNPARENTHESISED ON
                                                         * PURPOSE: `frames * EPD_MODEL_FRAME_MS` has to mean
                                                         * `frames * 1000 / 60` (integer math left to right), which is
                                                         * what every duration already recorded in logs/qemu-b651-epd.log
                                                         * was produced with -- gc16 is 24 * 1000 / 60 = 400 ms there
                                                         * (logs/qemu-b651-epd.log:148). Writing it as
                                                         * (1000 / 60) would make it 16 first and 24 * 16 = 384,
                                                         * quietly changing every timing and contradicting the log. A
                                                         * macro that is only ever used multiplied is a trap either
                                                         * way; this note is the guard against "tidying" it. */
static const struct {
	const char      *name;
	uint32_t        frames;                         /* how long the update takes */
	bool            flashes;                        /* whether it blanks the panel first */
} epd_model[] = {
	{ "gc16", 24, true },           /* full grey-level clear */
	{ "gl16", 16, false },          /* grey-level, no flash */
	{ "du",  6,  false },           /* direct update */
	{ "a2",  2,  false },           /* animation */
};

static int32_t epd_model_of(uint32_t waveform_mode);    /* waveform_mode -> epd_model[] index */
static void epd_start_model(int model, const struct hakios_epd_update *u);
static void epd_panel_bounds(uint32_t *width, uint32_t *height);
static void model_done(thread_call_param_t unused, thread_call_param_t arg);

/* The model backend's timer, allocated once at attach and re-armed for every update (epd_start_model() says why it is
 * one call and not one per update). Declared here, above epd_attach(), because that is where it is made. */
static thread_call_t epd_model_call;

/*
 * Whether to drive the MODEL instead of a controller: boot-arg epd_model=1, which the simulator's payload passes
 * and the phone's never will. It is a boot argument and not a guess at the machine, because the machine cannot be
 * asked: with no device tree to read, "there is no EPDC here" and "the phone's dtbo has not been read" look
 * identical from inside the kernel. Saying which backend to use is the caller's decision, so a run cannot end up
 * believing a timing table is a panel.
 */
static bool
epd_model_ok(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("epd_model", &on, sizeof(on)) && on != 0;
}

/*
 * The facts, then the panel: reset it, raise its rails, read the controller's version (EPDC.md:101).
 * On QEMU there is no controller and nothing to guess: the model backend is the panel, and it is the one that
 * runs. On the phone the three facts are still 0, and it says which one rather than inventing an address.
 *
 * The one thing this attach does that the skeleton did not: it allocates the model's thread call, ONCE. epd_start_model
 * re-arms that one call for every update (see the comment there for why a per-update allocation leaked the call
 * struct). Allocating it here rather than lazily at first use keeps the failure in one place -- a NULL model call is
 * "the simulator's timer could not be made", said at attach, instead of an update that silently completes instantly.
 */
static int
epd_attach(void)
{
	uint32_t width, height;

	if (epd_hw.base == 0 || epd_hw.size == 0) {
		if (epd_model_ok()) {
			epd_model_call = thread_call_allocate(model_done, NULL);
			if (epd_model_call == NULL) {
				printf("hakios-epd: no EPDC and no thread call to model it with; nothing is driven this run\n");
				return ENOTSUP;
			}
			epd_panel_bounds(&width, &height);
			/* one line, and it says the three things a reader of a boot log needs to know before trusting a
			 * timing: the panel the rects are bounded by, the table the frame counts come from, and that the
			 * table is a MODEL. A size of 0 is said as 0: "no framebuffer, so no bound" is a fact, not a hole. */
			printf("hakios-epd: no EPDC (the simulator has none), so the panel is MODELED: %ux%u, the epd.py "
			    "waveform table (gc16 %u, gl16 %u, du %u, a2 %u frames at %u ms). The marker, EBUSY and the wait "
			    "above it are the phone's\n",
			    width, height, epd_model[0].frames, epd_model[1].frames, epd_model[2].frames, epd_model[3].frames,
			    (uint32_t)EPD_MODEL_FRAME_MS);
			return 0;
		}
		printf("hakios-epd: the EPDC's address is not known: the phone's dtbo has not been read "
		    "(/eink, /epd_pmic_det; devices/b651/PHONE.md). Nothing is driven this run\n");
		return ENOTSUP;
	}
	/*
	 * With a base: map it (ml_io_map), reset the panel through the EPD PMIC (SYV776A at I2C 0x62, or
	 * tps65185: which one is a root-level question, PHONE.md:83-84), read its version register, and
	 * load the waveform blob from the `waveform` partition. The register layout and the blob's format
	 * are both unverified, so none of it is written here.
	 */
	return ENOTSUP;
}

/* The POWER_STATE_OFF -> ON half (EPDC.md:102). Writes nothing until the power sequence is known. */
static void
epd_powerup(void)
{
	/* the rails and VCOM through the EPD PMIC, in the vendor's order: order and values unknown, and a
	 * wrong one can damage the panel (EPDC.md:178). */
}

/* The ON -> OFF half (EPDC.md:103). Never called with a LUT in flight (EPDC.md:145). */
static void
epd_powerdown(void)
{
	/* the EPD PMIC again, in reverse; same unknowns as epd_powerup(). */
}

/*
 * What is ours to check about an update, before anything touches the panel.
 *
 * THE BOUNDS CHECK USED TO BE DEAD CODE, and this is the fix. It tested epd_hw.panel_width / panel_height, which are
 * 0 everywhere in this repository -- both `static` and never assigned (grep: `epd_hw.panel_` has no assignment), and
 * epd_hw.base is 0, so epd_attach() returns ENOTSUP before the block that would fill them ever runs. A user-supplied
 * rect was therefore bounded only by 2^32, on a panel of 1125x2436: left = 8000, top = 100 was a legal update, and
 * nothing between it and the controller said otherwise. That is the exact class of defect EPDC.md:47 warns about and
 * that a driver is supposed to refuse at the boundary, not hope the controller rejects.
 *
 * The real geometry is readable WITHOUT root, and this driver has it: the boot framebuffer is the panel -- the text
 * console draws into it, MiniIOMobileFramebuffer copies into it, and it is 1125x2436 in every log (qemu-b651-epd.log:
 * "MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1125x2436"). `vinfo` is that framebuffer, and it is what
 * hakios.virtio_gpu.c:963-964 and hakios.fb.c:92-93 already take their panel size from. So the limit is
 *
 *     epd_hw.panel_width  ? that : vinfo.v_width       (0 if there is no framebuffer at all)
 *
 * -- the controller's own stated limit when it is known, the framebuffer's otherwise. Both are read once per check;
 * neither is written from here, and the UNVERIFIED fields keep their meaning ("the controller's limit is unknown").
 *
 * This is a limit on the PANEL, not the controller's burst width (max_update_width), which is still 0 and still
 * means "split the update" rather than "refuse it" (EPDC.md:47,160). Refusing a rect the panel has is wrong; a
 * rect past the panel is not.
 */
static void
epd_panel_bounds(uint32_t *width, uint32_t *height)
{
	*width = epd_hw.panel_width != 0 ? epd_hw.panel_width : vinfo.v_width;
	*height = epd_hw.panel_height != 0 ? epd_hw.panel_height : vinfo.v_height;
}

static int
epd_check(const struct hakios_epd_update *u)
{
	uint32_t width, height;

	epd_panel_bounds(&width, &height);

	if (u->width == 0 || u->height == 0) {
		return EINVAL;
	}
	if ((uint64_t)u->left + u->width > UINT32_MAX || (uint64_t)u->top + u->height > UINT32_MAX) {
		return EINVAL;
	}
	if (u->update_mode != HAKIOS_EPD_UPDATE_PARTIAL && u->update_mode != HAKIOS_EPD_UPDATE_FULL) {
		return EINVAL;
	}
	if (u->marker == 0) {
		return EINVAL;
	}
	/* past the panel: a rectangle the panel cannot show. 64-bit arithmetic before the compare, because left and
	 * width are each a full uint32_t and their sum is not one. */
	if (width != 0 && height != 0 &&
	    ((uint64_t)u->left + u->width > width || (uint64_t)u->top + u->height > height)) {
		return EINVAL;
	}
	return 0;
}

/*
 * rect + waveform + mode -> an update on the panel (EPDC.md:106). The three steps, in the order they run on the
 * phone: the waveform select (mode and temperature -> a LUT number, from the blob's table: EPDC.md:158-159), the
 * collision check against the LUTs in flight (mxc_epdc_fb.c:949-955, :3139-3197), and programming the controller.
 *
 * Here the third step is the simulator's: the update is timed, and epd_complete() runs when it is over. On the
 * phone it is a register write and the interrupt that ends it; the two above it are the same either way.
 *
 * ORDER OF THE CHECKS IS PART OF THE CONTRACT, not an accident: the caller's own mistakes (EINVAL) are named before
 * "there is no controller" (ENOTSUP), so a bad request still reads as a bad request on a machine with no panel. That
 * is what the header says at `HAKIOS_EPD_UPDATE`, and what devices/b651/tests/epd_test.c checks.
 *
 * The marker and the start happen in ONE critical section. It used to be two -- a check, an unlock, then a second
 * check under a second lock -- and the first one was wholly subsumed by the second (the second refuses EBUSY for
 * *any* marker in flight, not just the caller's). Two locks around one decision is a shape that invites a later
 * reader to believe the first one is load-bearing; it was not.
 */
static int
epd_submit(const struct hakios_epd_update *u)
{
	int32_t model;
	int error;

	error = epd_check(u);
	if (error != 0) {
		return error;
	}
	/* the waveform select: which model the mode names, or the panel's default when it names none. Ahead of the
	 * controller check on purpose: the header promises a caller's own mistake is still named on a machine with
	 * no panel (see `HAKIOS_EPD_UPDATE` above), and a bad waveform id is one of those. It was behind it once,
	 * which made a bad id read as "no controller" on the phone. */
	model = epd_model_of(u->waveform_mode);
	if (model < 0) {
		return EINVAL;
	}
	if (!epd_attached) {
		return ENOTSUP;
	}
	lck_mtx_lock(&epd_lock);
	if (epd_marker_in_flight != 0) {
		/* one update at a time, as the phone's controller has one LUT per rect and this backend can only stand
		 * for one: the caller's marker is not started, so the answer is EBUSY and not a queue */
		lck_mtx_unlock(&epd_lock);
		return EBUSY;
	}
	epd_marker_in_flight = u->marker;
	lck_mtx_unlock(&epd_lock);
	epd_start_model(model, u);
	return 0;
}

/*
 * The waveform table's names and the caller's numbers: which of epd_model[]'s four the caller's waveform_mode
 * names, or -1 for one this driver will not guess at.
 *
 * THE MAPPING IS NOT A FACT AND MUST NOT BE READ AS ONE. Three numbering systems are in play and they disagree
 * (WAVEFORMS.md:55-80 says this at length, and it is worth reading before "fixing" any line below):
 *
 *   mxcfb canonical   INIT 0, DU 1, GC16 2, GC4 3, A2 4  (mxc_epdc_fb_test.c:64-68)
 *   Bigme's integers  4 (GC16/full), 132 (GU16), 1029 (handwrite), 178, 180  (EPDC.md:55,162)
 *   Allwinner's       GC16 0x04, A2 0x10, DU_RECT 0x402
 *
 * Two collisions follow, and they are real:
 *   - `1` is DU in the mxcfb vocabulary this ABI is designed after (EPDC.md:119) and a2 here. Both cannot be right.
 *     The table keeps a2 because that is what the simulator's runs and logs are labelled with
 *     (logs/qemu-b651-epd.log:144 `update 101: 32x32 at 0,0, a2 (33 ms)`, from waveform 1 in epd_test.c:189);
 *     changing it would relabel every already-captured run. On the phone the LUT decides, and that is the point at
 *     which this mapping becomes a fact.
 *   - `180` is recorded by EPDC.md:55 as one of Bigme's integers without saying which mode it is. Calling it `du`
 *     here is the least supported line in the table -- but a mode has to be named to be timed, and refusing it would
 *     make the vendor's own default a silent EINVAL.
 *
 * `1029` (Bigme's handwrite, the mode the vendor HAL uses for a pen) is deliberately NOT in the switch: the model
 * has no pen mode, and inventing one from the four it has would be the "guess at a number" the file's head warns
 * about. It is refused with EINVAL, by name of being absent, rather than mapped to something that is not it.
 *
 * What this is safe to be wrong about: the model's durations are the only thing the mapping selects, and those are
 * ASSUMED model numbers already (WAVEFORMS.md:163-166). Nothing here reaches a register, so a wrong name costs a
 * wrong timing in the simulator and nothing on the panel.
 */
static int32_t
epd_model_of(uint32_t waveform_mode)
{
	switch (waveform_mode) {
	case 0:         /* "the blob's default" -- the panel's own, whatever the LUT calls it */
	case 4:         /* EPDC.md:55 -- the one Android uses for a full refresh. CITED */
		return 0;                                       /* gc16 */
	case 132:       /* EPDC.md:55's GU16: a grey-level update with no flash. ASSUMED as gl16 */
		return 1;                                       /* gl16 */
	case 180:       /* EPDC.md:55 records it without naming the mode. ASSUMED as du (see above) */
		return 2;                                       /* du */
	case 1:         /* the collision: mxcfb's DU, this table's a2 (see above) */
		return 3;                                       /* a2 */
	default:
		return -1;      /* 1029 included, deliberately: see above */
	}
}

/*
 * The controller's done-interrupt, as the simulator has it: the update's time is up. Clears the marker and wakes
 * whoever is in epd_wait(), which is exactly what the phone's handler will do when it is connected (EPDC.md:107).
 *
 * THE MARKER IS AN ARGUMENT, and that is the fix rather than a tidiness. The version before this took none: it
 * cleared whatever marker was in flight. The caller (model_done) tested the marker, dropped the lock, and called
 * this, which took the lock again -- so between the two, the update in flight could have become a *different* one:
 * this timer firing late, after its own update had been cleared and a new one started, would have cleared the new
 * update's marker and woken its waiter. That waiter would then be told "the pixels are on the panel" while the ink
 * was still moving, which is the single thing HAKIOS_EPD_WAIT exists to prevent. Nothing keeps a thread call from
 * being that late -- its deadline gets timer slop added (kern/thread_call.c:1300-1310) -- so the check has to be
 * under the same lock as the clear, and the marker has to be carried to it.
 *
 * A marker that is not the one this call was armed for is not this call's to clear, and 0 is nobody's: an interrupt
 * for an update that has already been accounted for does nothing.
 */
static void
epd_complete(uint32_t marker)
{
	lck_mtx_lock(&epd_lock);
	if (epd_marker_in_flight == marker && marker != 0) {
		epd_marker_in_flight = 0;
		wakeup(&epd_marker_in_flight);
	}
	lck_mtx_unlock(&epd_lock);
}

/*
 * The model backend's timer. ONE thread_call, allocated once at attach and re-armed for every update, and the marker
 * rides in its second parameter -- not one allocation per update.
 *
 * That was the defect this replaces: epd_start_model() called thread_call_allocate() for each update and never called
 * thread_call_free(). thread_call_allocate takes a reference out of the thread_call zone and the allocation is
 * released only when that reference is dropped AND the call has finished (kern/thread_call.c:1013-1025, :1102-1130,
 * :1619-1640); an update that is allocated, fired, finished and never freed keeps its struct forever. A console
 * doing one update per keystroke is a slow leak of a kernel object, and nothing in the reported numbers would show
 * it. Re-arming one call is what the house's own timers do (drivers/input/hakios.input.c:103-105 re-enters and
 * cancels a single exit_call; drivers/gpu/hakios.virtio_gpu.c:1105-1109 allocates one gpu_call and re-enters it
 * every second).
 *
 * Re-entering a call that is already pending is allowed: the second enter moves its deadline and returns TRUE
 * (kern/thread_call.c:1196-1213, and _delayed_call_enqueue at :753: `old_queue != NULL` is the TRUE it returns). It
 * cannot happen here anyway -- one update at a time, so the call is never armed while armed -- but it is a safe
 * shape, not a lucky one.
 */
static void
model_done(thread_call_param_t unused __unused, thread_call_param_t arg)
{
	uint32_t marker = (uint32_t) (uintptr_t) arg;

	epd_complete(marker);
}

/* The update on the simulator's panel: it drives for as long as the waveform says, then epd_complete().
 *
 * epd_model_call cannot be NULL here: epd_attach() allocates it before it lets epd_attached become true, and
 * epd_submit() returns ENOTSUP while epd_attached is false, so an update only reaches this with the call made. The
 * skeleton had a NULL case here, from when the call was allocated per update and could fail per update; that branch
 * completed the update at once, which told the caller the pixels were on the panel when nothing had been driven.
 * Saying ENOTSUP at attach is the honest place for that failure, and it is where it is now.
 *
 * THE LOG SAYS `update mode full`, NOT `whole panel`, AND TWO FACTS IT USED TO CONFLATE ARE NOW TWO SUFFIXES.
 * gc16 flashes a rectangle -- it clears the pixels inside it -- while FULL is a statement about WHICH PIXELS the
 * update covers; a flashing waveform and a full-panel update are independent. The line used to take `whole panel`
 * from `epd_model[model].flashes`, so a gc16 update of a 32x32 rect read as "whole panel" (which is what
 * logs/qemu-b651-epd.log:148 records: `update 102: 32x32 at 0,0, gc16 (400 ms), whole panel`), and an a2 update of
 * the whole screen -- which is what the full-refresh test submits -- read as nothing at all. The rect printed is the
 * truth; a suffix that contradicts it is worse than no suffix. Both suffixes are now what they say: `update mode
 * full` from the mode the caller sent, `flashes` from the waveform. Neither is an instruction to the model, which
 * does not flash anything (WAVEFORMS.md:238 records the flash as a thing the model does not render).
 */
static void
epd_start_model(int model, const struct hakios_epd_update *u)
{
	uint64_t deadline;
	uint32_t ms = epd_model[model].frames * EPD_MODEL_FRAME_MS;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter1_delayed(epd_model_call, (thread_call_param_t) (uintptr_t) u->marker, deadline);
	printf("hakios-epd: update %u: %ux%u at %u,%u, %s (%u ms)%s%s\n", u->marker, u->width, u->height, u->left,
	    u->top, epd_model[model].name, ms, u->update_mode == HAKIOS_EPD_UPDATE_FULL ? ", update mode full" : "",
	    epd_model[model].flashes ? ", flashes" : "");
}

/* HAKIOS_EPD_WAIT: wait for the marker (0: any) to finish. epd_complete() is what finishes it. */
static int
epd_wait(uint32_t marker, uint32_t *collision)
{
	int error;

	if (!epd_attached) {
		return ENOTSUP;
	}
	lck_mtx_lock(&epd_lock);
	while (epd_marker_in_flight != 0 && (marker == 0 || marker == epd_marker_in_flight)) {
		error = msleep(&epd_marker_in_flight, &epd_lock, PZERO | PCATCH, "hakios-epd", NULL);
		if (error != 0) {
			lck_mtx_unlock(&epd_lock);
			return error;
		}
	}
	*collision = 0;
	lck_mtx_unlock(&epd_lock);
	return 0;
}

static int
epd_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
epd_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

/*
 * A stream of struct hakios_epd_update, each handled exactly as HAKIOS_EPD_UPDATE is, stopping at the first refused
 * record. What it REFUSES is worth stating, because returning 0 while discarding the caller's bytes is the one thing
 * this must not do:
 *
 *   - a stream that is not a whole number of records: the loop consumes the whole ones and then stops on the
 *     remainder, and the errno is EINVAL for the same reason -- the bytes past the last complete record cannot be
 *     a request, so they are refused rather than dropped. (A zero-length write is 0, as write(2) is for every other
 *     character device: nothing was asked and nothing is refused.) The check is ahead of the loop because a partial
 *     record at the END would otherwise be consumed by a uiomove that stops short and leaves `u` holding the previous
 *     record's tail -- a request the caller never made.
 */
static int
epd_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_epd_update u = { 0 };
	int error = 0;

	if (uio_resid(uio) != 0 && uio_resid(uio) % (user_ssize_t)sizeof(u) != 0) {
		return EINVAL;
	}
	while (uio_resid(uio) >= (user_ssize_t)sizeof(u)) {
		error = uiomove((caddr_t)&u, sizeof(u), uio);
		if (error != 0) {
			break;
		}
		error = epd_submit(&u);
		if (error != 0) {
			break;
		}
	}
	return error;
}

static int
epd_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
	switch (cmd) {
	case (u_long)HAKIOS_EPD_UPDATE:
		return epd_submit((const struct hakios_epd_update *)data);
	case (u_long)HAKIOS_EPD_WAIT:
		{
			struct hakios_epd_wait *wait = (struct hakios_epd_wait *)data;

			return epd_wait(wait->marker, &wait->collision);
		}
	case (u_long)HAKIOS_EPD_SET_TEMP:
		{
			int32_t temperature = *(const int32_t *)data;

			if (temperature == HAKIOS_EPD_TEMP_UNSET) {
				return EINVAL;
			}
			/* no range check: the range the panel may be driven over is in the waveform blob's
			 * temperature table, which is not known (EPDC.md:159) */
			lck_mtx_lock(&epd_lock);
			epd_temperature = temperature;
			lck_mtx_unlock(&epd_lock);
			printf("hakios-epd: the waveform temperature: %d C\n", temperature);
			return 0;
		}
	case (u_long)HAKIOS_EPD_POWERDOWN:
		{
			int32_t ms = *(const int32_t *)data;
			bool in_flight;

			if (ms < -1) {
				return EINVAL;
			}
			/*
			 * A FIGHT WITH AN UPDATE IS REFUSED, not waited out. EPDC.md:145 and :132 make it a rule of the
			 * state machine: idle is "no LUT active and no pending update", and a panel must never be powered
			 * down with a LUT in flight. The skeleton's comment said "nothing can be in flight here", which was
			 * true only because nothing could be in flight at all -- the model backend makes an update 400 ms
			 * long, so a caller's POWERDOWN(0) arriving mid-refresh is ordinary, not impossible. The delay is
			 * still recorded before the check, so a caller that set it to 0 gets the setting either way; what
			 * EBUSY refuses is the act, not the intent.
			 */
			lck_mtx_lock(&epd_lock);
			epd_powerdown_ms = ms;
			in_flight = epd_marker_in_flight != 0;
			lck_mtx_unlock(&epd_lock);
			if (ms == 0 && in_flight) {
				return EBUSY;
			}
			if (ms == 0) {
				epd_powerdown();
			}
			return 0;
		}
	default:
		return ENOTTY;
	}
}

static const struct cdevsw epd_cdevsw = {
	.d_open = epd_open, .d_close = epd_close, .d_read = eno_rdwrt, .d_write = epd_write,
	.d_ioctl = epd_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * Called from bsd_init once devfs is mounted (sys/hakios.epd.h), the same point /dev/hakios-touch is made at: a node
 * made before that mount is not in /dev, which is what a first version of this did from STARTUP and the open of it
 * failed EBADF.
 */
void
hakios_epd_init(void)
{
	uint32_t enabled = 0;
	int major;

	if (!PE_parse_boot_argn("epd", &enabled, sizeof(enabled)) || enabled == 0) {
		return;                 /* nothing is driven unless a boot-argument asks */
	}
	major = cdevsw_add(-1, &epd_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-epd") == NULL) {
		printf("hakios-epd: cannot create the device\n");
		return;
	}
	printf("hakios-epd: /dev/hakios-epd; %d C, powerdown after %d ms\n", epd_temperature, epd_powerdown_ms);
	if (epd_attach() != 0) {
		return;
	}
	epd_attached = true;
	epd_powerup();
	/* and then the init waveform, a full refresh, and a powerdown: mxc_epdc_fb.c:4246, EPDC.md:140.
	 * Deferred with the waveform blob. */
}
