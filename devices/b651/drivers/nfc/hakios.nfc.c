/*
 * /dev/hakios-nfc: the ST54 NFC controller + secure element, as a character device. Only with boot-arg nfc=1.
 *
 * WHAT THIS IS. The vendor's driver (drivers/nfc/st54-spi/st54spi.c, 1292 lines, in the phone's Android tree) is a
 * spidev-shaped character device with a power/reset state machine on one GPIO and an eSE in/out request. That state
 * machine -- not the SPI registers -- is what this file is: the same states, the same order, the same waits, so that
 * when the pin layer exists it is a backend and not a rewrite. This is the shape hakios.epd.c took for the panel.
 *
 * WHAT IS MODELED, AND WHY. QEMU has no NFC controller and no MediaTek SPI master, and the phone's own dtbo has not
 * been read, so which of spi0..spi7 the ST54 hangs off, its chip select, its IRQ and its reset GPIO are all unknown
 * (devices/b651/NFC.md). Boot-arg nfc_model=1 attaches a MODELED chip instead: powering it on answers, resetting it
 * answers, and a read returns a fixed answer to the one APDU that is checked. Everything above that -- the device
 * node, the ioctls, the power sequencing, the state transitions, the error codes -- is the code the phone will run.
 *
 * WHAT IS NOT HERE, and cannot be: the ST54's framing above SPI. The vendor's file is a passthrough (the framing is
 * in its firmware), so a byte-level conversation with a real part is not something this tree knows how to have. The
 * modeled chip answers in APDUs at the length-prefixed level, which is where the vendor's own userspace sits.
 *
 * ABI, after the vendor's (drivers/nfc/st21nfc-i2c/st21nfc.h, ST21NFC_MAGIC 0xEA) so that a userspace written for
 * the phone's Android stack has the same shape:
 *
 *   HAKIOS_NFC_POWER      _IOW('N', 1, int32_t)   1 = power on (eSE power request), 0 = power off
 *   HAKIOS_NFC_RESET      _IO('N', 2)             pulse the reset line
 *   HAKIOS_NFC_USE_ESE    _IOW('N', 3, uint32_t)  the caller is talking to the secure element, not the CLF
 *   HAKIOS_NFC_STATUS     _IOR('N', 4, struct hakios_nfc_status)
 *   read()/write()        one APDU frame each way, length-prefixed, as the vendor's device does
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/locks.h>
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

/* ---- the ABI -------------------------------------------------------------------------------------------- */
struct hakios_nfc_status {
	uint32_t powered;               /* the eSE/CLF is powered (1) or not (0) */
	uint32_t reset_count;           /* how many resets since the device was made */
	uint32_t ese_selected;          /* the last HAKIOS_NFC_USE_ESE value */
	uint32_t frames;                /* APDU frames written since the device was made */
	uint32_t modeled;               /* 1: the chip is the simulator's model, 0: a real one */
};

#define HAKIOS_NFC_POWER        _IOW('N', 1, int32_t)
#define HAKIOS_NFC_RESET        _IO('N', 2)
#define HAKIOS_NFC_USE_ESE      _IOW('N', 3, uint32_t)
#define HAKIOS_NFC_STATUS       _IOR('N', 4, struct hakios_nfc_status)

/* The largest frame the vendor's buffer takes (st54spi.c: bufsiz), and the length prefix it puts in front of one. */
#define NFC_FRAME_MAX           4096

/*
 * The facts the pin layer will need, and where they come from. Every field is 0 until it is known: a 0 is "not
 * known", and nfc_attach() names what is missing rather than inventing a number.
 */
static struct hakios_nfc_hw {
	uint32_t spi_master;            /* 0: UNVERIFIED. Which of spi0..spi7 (NFC.md); the dumps name the eight, not which */
	uint32_t cs;                    /* 0: UNVERIFIED. The chip select on it */
	uint32_t irq;                   /* 0: UNVERIFIED. /irq_nfc is a node name only (NFC.md) */
	uint32_t reset_gpio;            /* 0: UNVERIFIED. The vendor's "gpio-power_nreset" (st54spi.c:958) */
	uint32_t power_mode;            /* POWER_MODE_ST54H or _ST54J; the vendor's dtbo decides (st54spi.c:969-972) */
} nfc_hw;

#define NFC_POWER_MODE_ST54H    0
#define NFC_POWER_MODE_ST54J    1

static LCK_GRP_DECLARE(nfc_lck_grp, "hakios-nfc");
static LCK_MTX_DECLARE(nfc_lock, &nfc_lck_grp);

static bool nfc_attached;
static bool nfc_modeled;
static uint32_t nfc_powered;
static uint32_t nfc_reset_count;
static uint32_t nfc_ese_selected;
static uint32_t nfc_frames;
static uint8_t nfc_reply[NFC_FRAME_MAX];        /* the answer the last write got, for the next read */
static size_t nfc_reply_len;                    /* 0: nothing to hand back */

/*
 * The power sequence, in the vendor's order (st54spi.c:441-497):
 *
 *   on:  raise the line -> 1.0-1.5 ms -> NSS to the SPI function -> 4-5 ms -> the chip is up
 *   off: drop the line -> (ST54H: 2-4.5 ms for the CLF to see it fall) -> the chip is down
 *
 * The waits are real in the vendor's code and are what a transceive depends on: a frame sent inside them is lost.
 * In the simulator they are what the model's latency stands in for.
 */
static const struct {
	uint32_t on_raise_us;
	uint32_t on_settle_us;
	uint32_t off_fall_us;
} nfc_timing = { 1250, 4500, 3250 };

static void
nfc_delay_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* The reset line, the SPI function select: the two things the pin layer will drive. Nothing here has pins yet. */
static void
nfc_line_reset(uint32_t on)
{
	if (nfc_modeled) {
		return;                 /* the model answers whatever the line does; the state below is the point */
	}
	/* the phone: a GPIO write through the pin controller, which this kernel has no path to yet (NFC.md) */
	(void)on;
}

static void
nfc_power_on(void)
{
	nfc_line_reset(1);
	nfc_delay_us(nfc_timing.on_raise_us);
	/* ST54H only: the SPI is not to be touched between the fall and the rise, which is why the vendor
	 * nulls its spi pointer across the sequence (st54spi.c:425-436) */
	nfc_delay_us(nfc_timing.on_settle_us);
	nfc_powered = 1;
}

static void
nfc_power_off(void)
{
	nfc_line_reset(0);
	if (nfc_hw.power_mode == NFC_POWER_MODE_ST54H) {
		nfc_delay_us(nfc_timing.off_fall_us);
	}
	nfc_powered = 0;
}

/*
 * What the modeled chip answers. Two APDUs, the two every NFC stack sends first:
 *   SELECT by AID           00 A4 04 00 ...   -> 90 00 (done)
 *   GET DATA (UID)          80 CA 00 00       -> 90 00 with a UID
 * Anything else is 6A 82, the standard "file not found", so a caller cannot mistake the model for a real part that
 * happens to agree with it.
 */
static int
nfc_model_transceive(const uint8_t *out, size_t out_len, uint8_t *in, size_t *in_len)
{
	if (!nfc_powered) {
		return EBUSY;           /* powered down: the vendor's chip answers nothing either */
	}
	if (*in_len < 2) {
		return ENOMEM;
	}
	if (out_len >= 4 && out[0] == 0x00 && out[1] == 0xA4 && out[2] == 0x04) {
		in[0] = 0x90;
		in[1] = 0x00;
		*in_len = 2;
		return 0;
	}
	if (out_len >= 4 && out[0] == 0x80 && out[1] == 0xCA && out[2] == 0x00 && out[3] == 0x00) {
		in[0] = 0x00;           /* a "UID": fixed, so a test can assert on it */
		in[1] = 0x04;           /* length */
		in[2] = 0xD1;
		in[3] = 0x23;
		in[4] = 0x45;
		in[5] = 0x67;
		in[6] = 0x90;
		in[7] = 0x00;
		*in_len = 8;
		return 0;
	}
	in[0] = 0x6A;
	in[1] = 0x82;
	*in_len = 2;
	return 0;
}

static int
nfc_power(uint32_t on)
{
	lck_mtx_lock(&nfc_lock);
	if (on) {
		if (!nfc_powered) {
			nfc_power_on();
		}
	} else if (nfc_powered) {
		nfc_power_off();
	}
	lck_mtx_unlock(&nfc_lock);
	return 0;
}

static int
nfc_reset(void)
{
	lck_mtx_lock(&nfc_lock);
	if (!nfc_attached) {
		lck_mtx_unlock(&nfc_lock);
		return ENOTSUP;
	}
	nfc_line_reset(0);
	nfc_delay_us(200);
	nfc_line_reset(1);
	nfc_delay_us(nfc_timing.on_raise_us);
	nfc_reset_count++;
	nfc_powered = 1;                /* a reset line that is also the power line brings it back up */
	lck_mtx_unlock(&nfc_lock);
	printf("hakios-nfc: reset %u\n", nfc_reset_count);
	return 0;
}

/* One frame out, one back: the vendor's device is a passthrough, and so is this one where it has a chip to pass to. */
static int
nfc_transceive(struct uio *uio)
{
	uint8_t out[NFC_FRAME_MAX], in[NFC_FRAME_MAX];
	size_t out_len, in_len;
	user_ssize_t resid = uio_resid(uio);
	int error;

	if (resid <= 0 || resid > NFC_FRAME_MAX) {
		return EINVAL;
	}
	out_len = (size_t)resid;
	error = uiomove((caddr_t)out, (int)out_len, uio);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(&nfc_lock);
	if (!nfc_modeled) {
		lck_mtx_unlock(&nfc_lock);
		/* a real ST54: the frame goes out over SPI, and this kernel has no path to one yet (NFC.md) */
		return ENOTSUP;
	}
	in_len = sizeof(in);
	error = nfc_model_transceive(out, out_len, in, &in_len);
	if (error != 0) {
		lck_mtx_unlock(&nfc_lock);
		return error;
	}
	nfc_frames++;
	/* the reply waits for read(), as it does on the vendor's device: a write's buffer is consumed by the uiomove
	 * above, so writing the answer back into it would overwrite the caller's own request with a shorter frame and
	 * leave the rest of it stale. A transceive is a write then a read, which is also what SPI does. */
	memcpy(nfc_reply, in, in_len);
	nfc_reply_len = in_len;
	lck_mtx_unlock(&nfc_lock);
	return 0;
}
static int
nfc_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t sent = uio_resid(uio);
	int error = nfc_transceive(uio);

	/* write() hands back how many bytes it took, as every device does; the frame is all of them */
	return error == 0 ? (int)sent : error;
}

static int
nfc_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t want = uio_resid(uio);
	int error;

	lck_mtx_lock(&nfc_lock);
	if (nfc_reply_len == 0) {
		lck_mtx_unlock(&nfc_lock);
		/* nothing to hand back: the vendor's read() asks the chip for bytes, and this one has no question to put
		 * to it. Answering ENOTSUP rather than a short read keeps a caller from taking "0 bytes" for a reply. */
		return ENOTSUP;
	}
	if ((size_t)want > nfc_reply_len) {
		want = (user_ssize_t)nfc_reply_len;     /* a short read, as any device does: the frame's own length */
	}
	error = uiomove((caddr_t)nfc_reply, (int)want, uio);
	if (error == 0) {
		nfc_reply_len = 0;              /* one reply per question */
	}
	lck_mtx_unlock(&nfc_lock);
	return error;
}

static int
nfc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
	switch (cmd) {
	case HAKIOS_NFC_POWER:
		return nfc_power(*(const int32_t *)data ? 1 : 0);
	case HAKIOS_NFC_RESET:
		return nfc_reset();
	case HAKIOS_NFC_USE_ESE:
		lck_mtx_lock(&nfc_lock);
		nfc_ese_selected = *(const uint32_t *)data;
		lck_mtx_unlock(&nfc_lock);
		printf("hakios-nfc: the caller is talking to the %s\n", nfc_ese_selected ? "secure element" : "CLF");
		return 0;
	case HAKIOS_NFC_STATUS:
		{
			struct hakios_nfc_status *s = (struct hakios_nfc_status *)data;

			s->powered = nfc_powered;
			s->reset_count = nfc_reset_count;
			s->ese_selected = nfc_ese_selected;
			s->frames = nfc_frames;
			s->modeled = nfc_modeled ? 1 : 0;
			return 0;
		}
	default:
		return ENOTTY;
	}
}

static int
nfc_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return nfc_attached ? 0 : ENXIO;
}

static int
nfc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw nfc_cdevsw = {
	.d_open = nfc_open, .d_close = nfc_close, .d_read = nfc_read, .d_write = nfc_write,
	.d_ioctl = nfc_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The facts, then the chip. On QEMU there is no controller and no SPI master, so the model is the chip and it says
 * so; on the phone the four facts above are still 0 and this names the first one rather than guessing a pin.
 */
static int
nfc_attach(void)
{
	uint32_t on = 0;

	if (PE_parse_boot_argn("nfc_model", &on, sizeof(on)) && on != 0) {
		nfc_modeled = true;
		printf("hakios-nfc: no ST54 on this machine (QEMU has no NFC and no MediaTek SPI master), so the chip is "
		    "MODELED: power, reset, the eSE request and an APDU transceive answer, and the state machine above "
		    "them is the phone's\n");
		return 0;
	}
	if (nfc_hw.spi_master == 0 || nfc_hw.cs == 0 || nfc_hw.irq == 0 || nfc_hw.reset_gpio == 0) {
		printf("hakios-nfc: the ST54's pins are not known: which of spi0..spi7, its chip select, /irq_nfc's number "
		    "and its reset GPIO all come from the phone's dtbo, which has not been read (devices/b651/NFC.md). "
		    "Nothing is driven this run\n");
		return ENOTSUP;
	}
	return ENOTSUP;
}

void
hakios_nfc_init(void)
{
	uint32_t enabled = 0;
	int major;

	if (!PE_parse_boot_argn("nfc", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	major = cdevsw_add(-1, &nfc_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-nfc") == NULL) {
		printf("hakios-nfc: cannot create the device\n");
		return;
	}
	printf("hakios-nfc: /dev/hakios-nfc\n");
	if (nfc_attach() != 0) {
		return;
	}
	nfc_attached = true;
}
