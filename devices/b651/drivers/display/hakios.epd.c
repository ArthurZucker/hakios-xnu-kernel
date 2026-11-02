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
 *     - the panel's geometry: /eink names eink_fb_width, eink_fb_height, eink_width and eink_height, and
 *       their values are not readable without root (PHONE.md:58,138-139). Until they are, no rect can be
 *       checked against the panel -- epd_check() checks only what is ours to check.
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
 *       0        the update was accepted (it is queued behind the waveform select; nothing in this
 *                skeleton gets this far)
 *       ENOTSUP  there is no controller to drive -- what a well-formed update gets on every run of this
 *                skeleton
 *       EINVAL   a rect with no area, a coordinate that runs past 2^32, marker 0, or an update_mode that
 *                is neither PARTIAL nor FULL (checked before the controller is looked at, so that a
 *                caller's own mistake is still named when there is no panel to drive)
 *       EBUSY    an update with that marker is already in flight (one marker at a time). The marker
 *                is only ever set once a LUT has started, so in this skeleton this cannot happen
 *   HAKIOS_EPD_WAIT       _IOWR('E', 2, struct hakios_epd_wait)
 *       0        the update at that marker has finished; `collision` says which LUTs were in the way.
 *                Nothing in this skeleton sets a marker in flight, so with a controller this would
 *                return at once with `collision` 0 -- falsely saying the pixels are on the panel. It
 *                answers ENOTSUP today for exactly that reason
 *       ENOTSUP  there is no controller, so no update can be in flight
 *       EINTR    a signal arrived while waiting (msleep's PCATCH: ERESTART too, for a signal that
 *                does not interrupt; /dev/hakios-touch's read passes both on in the same way)
 *   HAKIOS_EPD_SET_TEMP   _IOW('E', 3, int32_t)
 *       0        stored, for the waveform select that does not exist yet
 *       EINVAL   HAKIOS_EPD_TEMP_UNSET came in as a temperature
 *   HAKIOS_EPD_POWERDOWN  _IOW('E', 4, int32_t)
 *       0        the delay was recorded (ms; -1 = never); if it was 0 the panel was powered down, which
 *                with no controller means nothing was done
 *       EINVAL   a delay below -1
 *   anything else                                                        ENOTTY
 *
 *   write() takes a stream of struct hakios_epd_update, each one handled as HAKIOS_EPD_UPDATE is, and
 *   stops at the first refused record (the same shape as /dev/hakios-touch's writes). read() is not
 *   implemented (eno_rdwrt): the answers are the ioctls'.
 *
 * WHAT IS DELIBERATELY ABSENT (EPDC.md:124-127): waveform-mode upload (MXCFB_SET_WAVEFORM_MODES),
 *   auto-update mode, update scheme, dithering, alt-buffer, GET_WORK_BUFFER. Each is a caller-side feature
 *   laid over the four commands above; adding one before a full refresh works is how this gate never
 *   closes. Also absent, and for a reason of this file's own: epd_complete(), the LUT-done handler of
 *   EPDC.md:107. There is no interrupt to attach it to -- the controller's IRQ is one of the unknowns
 *   above -- and an unconnected handler would be dead code. When the interrupt exists it wakes the
 *   waiters of HAKIOS_EPD_WAIT and clears the marker in flight, exactly as it stands described there.
 *
 * Status: RUNS IN THE SIMULATOR. `epd=1 epd_model=1` on qemu-b651 attaches the model backend: an update drives for
 *   the waveform's time and then completes, so the ABI, the marker in flight, EBUSY, epd_wait()'s sleep and wake,
 *   the collision bitmap and the update/powerdown ordering are all exercised on a machine that cannot be hurt.
 *   What is NOT exercised is everything below that: no register, no LUT, no interrupt, no PMIC, no VCOM.
 *   Not run on the phone. `epd=1` alone there still reaches epd_attach()'s ENOTSUP, because epd_model=1 is only in
 *   the simulator's payload cmdline. What comes next, in order, is EPDC.md:200-214: the phone's dtbo (/eink,
 *   /epd_pmic_det) for the controller, and the `waveform` and `vcom` partitions for the LUTs and the per-unit VCOM
 *   -- then `epd_hw` below is filled in and the register backend is written beside this one.
 */
#include <stdint.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
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
#define EPD_MODEL_FRAME_MS      1000 / 60               /* a waveform frame at the panel's 60 Hz */
static const struct {
	const char      *name;
	uint32_t        frames;                         /* how long the update takes */
	bool            flashes;                        /* whether it blanks the panel first */
	uint8_t         residue;                        /* percent of ink the previous picture leaves (0-100) */
} epd_model[] = {
	{ "gc16", 24, true,  0 },       /* full grey-level clear */
	{ "gl16", 16, false, 6 },       /* grey-level, no flash */
	{ "du",  6,  false, 22 },       /* direct update */
	{ "a2",  2,  false, 45 },       /* animation */
};

static int32_t epd_model_of(uint32_t waveform_mode);    /* waveform_mode -> epd_model[] index */
static void epd_start_model(int model, const struct hakios_epd_update *u);

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
 */
static int
epd_attach(void)
{
	if (epd_hw.base == 0 || epd_hw.size == 0) {
		if (epd_model_ok()) {
			printf("hakios-epd: no EPDC (the simulator has none), so the panel is MODELED: an update takes the "
			    "waveform's time and then completes. The marker, EBUSY and the wait above it are the phone's\n");
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

/* What is ours to check about an update, before anything touches the panel. */
static int
epd_check(const struct hakios_epd_update *u)
{
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
	/* the panel's own bounds, once /eink's width and height are read (PHONE.md:58,138): then a rect that
	 * runs past them is EINVAL here. Checked against the controller's window too, and split past
	 * epd_hw.max_update_width (EPDC.md:47,160). */
	if (epd_hw.panel_width != 0 && epd_hw.panel_height != 0 &&
	    ((uint64_t)u->left + u->width > epd_hw.panel_width || (uint64_t)u->top + u->height > epd_hw.panel_height)) {
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
 */
static int
epd_submit(const struct hakios_epd_update *u)
{
	int error;

	error = epd_check(u);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(&epd_lock);
	if (u->marker == epd_marker_in_flight) {
		lck_mtx_unlock(&epd_lock);
		return EBUSY;
	}
	lck_mtx_unlock(&epd_lock);
	if (!epd_attached) {
		return ENOTSUP;
	}
	/* the waveform select: which model the mode names, or the panel's default when it names none */
	error = epd_model_of(u->waveform_mode);
	if (error < 0) {
		return EINVAL;
	}
	lck_mtx_lock(&epd_lock);
	if (epd_marker_in_flight != 0) {
		/* one update at a time, as the phone's controller has one LUT per rect and this backend can only
		 * stand for one: the caller's marker is not started, so the answer is EBUSY and not a queue */
		lck_mtx_unlock(&epd_lock);
		return EBUSY;
	}
	epd_marker_in_flight = u->marker;
	lck_mtx_unlock(&epd_lock);
	epd_start_model(error, u);
	return 0;
}

/* The waveform table's names and the caller's numbers. 0 is the panel's default; the others are Android's
 * integers as EPDC.md:55,162 records them, which have no LUT to point at on the phone yet but do have a model
 * here. Anything else is refused rather than guessed at. */
static int32_t
epd_model_of(uint32_t waveform_mode)
{
	switch (waveform_mode) {
	case 0:
	case 4:         /* EPDC.md:55 -- the one Android uses for a full refresh */
		return 0;                                       /* gc16 */
	case 132:       /* ditto for a grey-level update with no flash */
		return 1;                                       /* gl16 */
	case 180:       /* ditto for a direct update */
		return 2;                                       /* du */
	case 1:         /* ditto for animation */
		return 3;                                       /* a2 */
	default:
		return -1;
	}
}

/*
 * The controller's done-interrupt, as the simulator has it: the update's time is up. Wakes whoever is in epd_wait()
 * and clears the marker, which is exactly what the phone's handler will do when it is connected (EPDC.md:107).
 */
static void
epd_complete(void)
{
	lck_mtx_lock(&epd_lock);
	epd_marker_in_flight = 0;
	wakeup(&epd_marker_in_flight);
	lck_mtx_unlock(&epd_lock);
}

static void
model_done(thread_call_param_t arg, thread_call_param_t unused __unused)
{
	uint32_t marker = (uint32_t) (uintptr_t) arg;

	lck_mtx_lock(&epd_lock);
	if (epd_marker_in_flight == marker) {
		lck_mtx_unlock(&epd_lock);
		epd_complete();
		return;
	}
	lck_mtx_unlock(&epd_lock);
}

/* The update on the simulator's panel: it drives for as long as the waveform says, then epd_complete(). */
static void
epd_start_model(int model, const struct hakios_epd_update *u)
{
	thread_call_t call;
	uint64_t deadline;

	call = thread_call_allocate(model_done, (thread_call_param_t) (uintptr_t) u->marker);
	if (call == NULL) {
		epd_complete();         /* nothing to wait on: the update is over at once, and says so */
		return;
	}
	clock_interval_to_deadline(epd_model[model].frames * EPD_MODEL_FRAME_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
	printf("hakios-epd: update %u: %ux%u at %u,%u, %s (%u ms)%s\n", u->marker, u->width, u->height, u->left, u->top,
	    epd_model[model].name, epd_model[model].frames * EPD_MODEL_FRAME_MS,
	    epd_model[model].flashes ? ", whole panel" : "");
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

static int
epd_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_epd_update u = { 0 };
	int error = 0;

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

			if (ms < -1) {
				return EINVAL;
			}
			lck_mtx_lock(&epd_lock);
			epd_powerdown_ms = ms;
			lck_mtx_unlock(&epd_lock);
			if (ms == 0) {
				/* nothing can be in flight here, or this skeleton would have said so above */
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
