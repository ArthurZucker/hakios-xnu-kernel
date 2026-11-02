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
 * answers, and a read returns a fixed answer to the APDUs that are checked, including the first three of a payment
 * conversation (SELECT PPSE, SELECT AID, GET PROCESSING OPTIONS: the templates and their bytes are below, and
 * devices/b651/WALLET.md says exactly what they are and are not). Everything above that -- the device node, the
 * ioctls, the power sequencing, the state transitions, the error codes -- is the code the phone will run.
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
 *   HAKIOS_NFC_CARD       _IOW('N', 5, uint32_t)  MODEL ONLY: 1 = a card is presented, 0 = the wallet has none
 *   read()/write()        one APDU frame each way, length-prefixed, as the vendor's device does
 *
 * HAKIOS_NFC_CARD is not a hardware control. An ST54 has no "there is a card" register: on the phone the card is
 * either in the field or the terminal hears nothing at all. It exists so a terminal written against this device can
 * be driven through the case where the wallet holds no card, which otherwise has no answer to test against
 * (devices/b651/WALLET.md). On a real chip it is refused with ENOTSUP rather than quietly doing nothing.
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
	/*
	 * No card_present here on purpose: this struct's size is half of HAKIOS_NFC_STATUS's _IOR number, so adding a
	 * field would change the ioctl and every caller built against it. The card's state is read through the APDU
	 * exchange itself -- a SELECT with no card answers 6A 82 (WALLET.md).
	 */
};

#define HAKIOS_NFC_POWER        ((u_long)_IOW('N', 1, int32_t))
#define HAKIOS_NFC_RESET        ((u_long)_IO('N', 2))
#define HAKIOS_NFC_USE_ESE      ((u_long)_IOW('N', 3, uint32_t))
#define HAKIOS_NFC_STATUS       ((u_long)_IOR('N', 4, struct hakios_nfc_status))
#define HAKIOS_NFC_CARD         ((u_long)_IOW('N', 5, uint32_t))

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
static bool nfc_card_present = true;            /* MODEL ONLY: HAKIOS_NFC_CARD; see the ABI comment above */
static bool nfc_app_selected;                   /* the last SELECT found the Visa AID; GET PROCESSING OPTIONS needs it */

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
	nfc_app_selected = false;       /* a chip that lost power has no selected application */
}

/*
 * What the modeled chip answers. Two APDUs, the two every NFC stack sends first:
 *   SELECT by AID           00 A4 04 00 ...   -> 90 00 (done)
 *   GET DATA (UID)          80 CA 00 00       -> 90 00 with a UID
 * Anything else is 6A 82, the standard "file not found", so a caller cannot mistake the model for a real part that
 * happens to agree with it.
 *
 * ---- the payment conversation (devices/b651/WALLET.md) ----
 *
 * A contactless terminal's first three APDUs, in the order it sends them. The responses are real ISO 7816-4 /
 * EMV book 1 TLV: every tag and length below was computed, not eyeballed, and the emulator test
 * devices/b651/tests/wallet_test.c asserts the exact bytes. What is NOT real is everything inside them -- there is
 * no card, no key, no cryptogram, and no amount. See WALLET.md before reading any of this as a wallet.
 *
 *   SELECT PPSE   00 A4 04 00 0E "2PAY.SYS.DDF01" 00
 *     -> 6F FCI: 84 (DF name) + A5 -> BF0C -> 61 (one app) -> 4F (Visa AID), 50 (label), 87 (priority 1), 90 00
 *     This says only "one payment application is here and it is Visa". SW 9000.
 *
 *   SELECT AID    00 A4 04 00 07 A0 00 00 00 03 10 10 00
 *     -> 6F FCI: 84 (AID) + A5 -> 50 label, 87 priority, 5F2D language, 9F38 PDOL, 90 00
 *     The PDOL tells the terminal which data to put in the next command. Ours asks for nine tags (TTQ, amount
 *     authorised, amount other, country code, TVR, currency, date, transaction type, unpredictable number) and the
 *     model does not care what values arrive -- see nfc_model_gpo().
 *
 *   GET PROCESSING OPTIONS  80 A8 00 00 Lc 83 Len <PDOL data> 00
 *     -> 77 template: 82 (AIP 0000: NO auth method claimed), 94 (AFL: one entry, SFI 1, records 1..1, 0 offline auth
 *        records), 90 00
 *     The AIP/AFL are format-correct and lead nowhere: the AIP claims no authentication method because the model has
 *     no key to certify anything with, and there is no record 1 and no READ RECORD behind the AFL, because a read of
 *     SFI 1 record 1 is deeper than this simulation goes (it would need a file system in the model).
 *
 * Not modeled, deliberately: no SELECT of anything but these two names, no READ RECORD, no GENERATE AC (the
 * cryptogram that a real terminal asks for -- nothing here can compute one), no PIN, no terminal risk management.
 */
static const uint8_t nfc_ppse_name[] = { 0x32, 0x50, 0x41, 0x59, 0x2E, 0x53, 0x59, 0x53, 0x2E, 0x44, 0x44,
					 0x46, 0x30, 0x31 };   /* "2PAY.SYS.DDF01" */
static const uint8_t nfc_visa_aid[] = { 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10 };

/*
 * SELECT "2PAY.SYS.DDF01" -> the PPSE FCI, then 90 00. 52 bytes with the status word:
 *
 *   6F 30                                 6F, 48 bytes: the FCI template
 *     84 0E 32 50 41 59 2E 53 59 53 2E 44 44 46 30 31     84: DF name = "2PAY.SYS.DDF01"
 *     A5 1E                                A5: FCI proprietary template
 *       BF0C 1B                           BF0C: FCI issuer discretionary data
 *         61 19                         61: one application template, 25 bytes
 *           4F 07 A0 00 00 00 03 10 10  4F: ADF name (the Visa AID)
 *           50 0B "VISA CREDIT"          50: application label, 11 bytes
 *           87 01 01                     87: application priority indicator, 1 (highest)
 *   90 00                                 SW_NO_ERROR
 */
static const uint8_t nfc_ppse_fci[] = {
	0x6F, 0x30,
	0x84, 0x0E, 0x32, 0x50, 0x41, 0x59, 0x2E, 0x53, 0x59, 0x53, 0x2E, 0x44, 0x44, 0x46, 0x30, 0x31,
	0xA5, 0x1E,
	0xBF, 0x0C, 0x1B,
	0x61, 0x19,
	0x4F, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10,
	0x50, 0x0B, 0x56, 0x49, 0x53, 0x41, 0x20, 0x43, 0x52, 0x45, 0x44, 0x49, 0x54,
	0x87, 0x01, 0x01,
	0x90, 0x00
};

/*
 * SELECT A0000000031010 -> the Visa FCI with a PDOL, then 90 00. 96 bytes with the status word:
 *
 *   6F 5C                                 FCI template, 92 bytes
 *     84 07 A0 00 00 00 03 10 10         DF name = the AID that was selected
 *     A5 51                             FCI proprietary template, 81 bytes
 *       50 0B "VISA CREDIT"              application label
 *       87 01 01                         priority
 *       5F 2D 02 65 6E                   5F2D: language preference, "en"
 *       9F 38 39                        9F38: PDOL, 57 bytes of tag/length pairs (all length 0 = "terminal fills")
 *         9F66 04   TTQ (terminal transaction qualifiers, 4 bytes)
 *         9F02 06   amount, authorised (6-byte BCD)
 *         9F03 06   amount, other (6-byte BCD)
 *         9F1A 02   terminal country code
 *         95    05   terminal verification results (TVR)
 *         5F2A 02   transaction currency code
 *         9A    03   transaction date (YYMMDD BCD)
 *         9C    01   transaction type
 *         9F37 04   unpredictable number
 *   90 00
 */
static const uint8_t nfc_visa_fci[] = {
	0x6F, 0x5C, 0x84, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0xA5,
	0x51, 0x50, 0x0B, 0x56, 0x49, 0x53, 0x41, 0x20, 0x43, 0x52, 0x45, 0x44,
	0x49, 0x54, 0x87, 0x01, 0x01, 0x5F, 0x2D, 0x02, 0x65, 0x6E, 0x9F, 0x38,
	0x39, 0x9F, 0x66, 0x04, 0x00, 0x00, 0x00, 0x00, 0x9F, 0x02, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x9F, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x9F, 0x1A, 0x02, 0x00, 0x00, 0x95, 0x05, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x5F, 0x2A, 0x02, 0x00, 0x00, 0x9A, 0x03, 0x00, 0x00, 0x00,
	0x9C, 0x01, 0x00, 0x9F, 0x37, 0x04, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00,
};

/*
 * GET PROCESSING OPTIONS -> the response template, then 90 00. 14 bytes with the status word:
 *
 *   77 0A         77 (response message template, format 1), 10 bytes
 *     82 02 00 00 82: AIP, 2 bytes, all zero: NO authentication method claimed. A real card sets a bit here for
 *                 SDA, DDA or CDA -- each of which means "I can prove I am a card with a key". This model has no
 *                 keys and cannot sign, so the only truthful value is none. (It also means the exact bit positions,
 *                 which this tree has not checked against EMV Book 3, cannot be wrong.) A terminal that insists on
 *                 offline authentication would refuse this card, correctly.
 *     94 04 08 01 01 00   94: AFL, one entry: SFI 1, first record 1, last record 1, 0 offline auth records.
 *                 Correct in form, empty in content: the model has no record 1 to read (see above).
 *   90 00
 */
static const uint8_t nfc_gpo[] = {
	0x77, 0x0A,
	0x82, 0x02, 0x00, 0x00,
	0x94, 0x04, 0x08, 0x01, 0x01, 0x00,
	0x90, 0x00
};

static bool
nfc_bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

static int
nfc_model_reply(const uint8_t *tpl, size_t tpl_len, uint8_t *in, size_t *in_len)
{
	if (*in_len < tpl_len) {
		return ENOMEM;
	}
	memcpy(in, tpl, tpl_len);
	*in_len = tpl_len;
	return 0;
}

/* SELECT [00 A4 04 00 Lc data ...]: the two names above are the only ones the model holds. */
static int
nfc_model_select(const uint8_t *out, size_t out_len, uint8_t *in, size_t *in_len)
{
	size_t lc, available;

	if (out_len == 4) {
		/* a bare SELECT with no Lc and no data: not a payment conversation at all. It is what the driver's own
		 * test sends to prove a frame round-trips, so it keeps the answer it always had. */
		in[0] = 0x90;
		in[1] = 0x00;
		*in_len = 2;
		return 0;
	}
	if (out_len < 5) {
		return EINVAL;          /* a SELECT header with no Lc byte in the frame */
	}
	lc = out[4];
	available = out_len - 5;        /* after the 5-byte header (Le included in out_len, so this may be one long) */
	if (lc > available) {
		return EINVAL;          /* Lc longer than the frame: a malformed APDU */
	}
	if (lc == sizeof(nfc_ppse_name) && nfc_bytes_equal(&out[5], nfc_ppse_name, lc)) {
		nfc_app_selected = false;       /* the PPSE is a directory, not the application itself */
		return nfc_model_reply(nfc_ppse_fci, sizeof(nfc_ppse_fci), in, in_len);
	}
	if (lc == sizeof(nfc_visa_aid) && nfc_bytes_equal(&out[5], nfc_visa_aid, lc)) {
		nfc_app_selected = true;
		return nfc_model_reply(nfc_visa_fci, sizeof(nfc_visa_fci), in, in_len);
	}
	return 1;                       /* "not one of mine": the caller answers 6A 82 */
}

/*
 * GET PROCESSING OPTIONS [80 A8 00 00 Lc 83 Len <PDOL data> Le]. The PDOL data is checked for shape, not content:
 * a real card builds the cryptogram input from those values, and this one has nothing to build it from. Requiring
 * the 83 tag and a length that fits the frame keeps a terminal that sends the wrong thing from being answered as if
 * it had sent the right thing.
 */
static int
nfc_model_gpo(const uint8_t *out, size_t out_len, uint8_t *in, size_t *in_len)
{
	size_t lc;

	if (!nfc_app_selected) {
		return 2;               /* "no application selected": the caller answers 69 85 */
	}
	if (out_len < 7) {
		return EINVAL;
	}
	lc = out[4];
	if (lc < 2 || lc > out_len - 5) {
		return EINVAL;
	}
	if (out[5] != 0x83) {
		return 6;               /* the command data must be a PDOL list tagged 83: the caller answers 6A 80 */
	}
	if ((size_t)out[6] + 2 > lc) {
		return EINVAL;          /* the 83 length runs past Lc */
	}
	return nfc_model_reply(nfc_gpo, sizeof(nfc_gpo), in, in_len);
}

static int
nfc_model_transceive(const uint8_t *out, size_t out_len, uint8_t *in, size_t *in_len)
{
	int ret;

	if (!nfc_powered) {
		return EBUSY;           /* powered down: the vendor's chip answers nothing either */
	}
	if (*in_len < 2) {
		return ENOMEM;
	}
	if (out_len >= 4 && out[0] == 0x00 && out[1] == 0xA4 && out[2] == 0x04) {
		/* A SELECT is the first thing a terminal sends, so it is the one that finds no card if there is none.
		 * A real terminal in the field sees no answer at all; here it gets 6A 82, "file not found", which is
		 * what the CLF hands up when the eSE has nothing to select. The choice is documented in WALLET.md. */
		if (!nfc_card_present) {
			in[0] = 0x6A;
			in[1] = 0x82;
			*in_len = 2;
			return 0;
		}
		ret = nfc_model_select(out, out_len, in, in_len);
		if (ret != 1) {
			return ret;     /* 0: the template is in the reply; anything else: an error for the caller */
		}
		in[0] = 0x6A;           /* the standard "file not found": nothing by that name on this card */
		in[1] = 0x82;
		*in_len = 2;
		return 0;
	}
	if (out_len >= 4 && out[0] == 0x80 && out[1] == 0xA8 && out[2] == 0x00 && out[3] == 0x00) {
		if (!nfc_card_present) {
			in[0] = 0x6A;
			in[1] = 0x82;
			*in_len = 2;
			return 0;
		}
		ret = nfc_model_gpo(out, out_len, in, in_len);
		if (ret == 0) {
			return 0;
		}
		if (ret == 2) {
			in[0] = 0x69;   /* SW_COMMAND_NOT_ALLOWED: GPO before the application was selected */
			in[1] = 0x85;
			*in_len = 2;
			return 0;
		}
		if (ret == 6) {
			in[0] = 0x6A;   /* SW_WRONG_DATA: Lc did not carry an 83 PDOL list */
			in[1] = 0x80;
			*in_len = 2;
			return 0;
		}
		return ret;
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
	nfc_app_selected = false;       /* a reset drops whatever was selected, as a cold card has nothing selected */
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
	case (u_long)HAKIOS_NFC_POWER:
		return nfc_power(*(const int32_t *)data ? 1 : 0);
	case (u_long)HAKIOS_NFC_RESET:
		return nfc_reset();
	case (u_long)HAKIOS_NFC_USE_ESE:
		lck_mtx_lock(&nfc_lock);
		nfc_ese_selected = *(const uint32_t *)data;
		lck_mtx_unlock(&nfc_lock);
		printf("hakios-nfc: the caller is talking to the %s\n", nfc_ese_selected ? "secure element" : "CLF");
		return 0;
	case (u_long)HAKIOS_NFC_CARD:
		/*
		 * MODEL ONLY. A real ST54 has no such control, and pretending it does would let a caller believe it had
		 * put a card in the field; it is refused rather than ignored. A card going away also drops the selected
		 * application, which is what removal does to a real chip's state.
		 */
		if (!nfc_modeled) {
			return ENOTSUP;
		}
		lck_mtx_lock(&nfc_lock);
		nfc_card_present = *(const uint32_t *)data ? true : false;
		nfc_app_selected = false;
		lck_mtx_unlock(&nfc_lock);
		printf("hakios-nfc: MODEL: card %s\n", nfc_card_present ? "present" : "absent");
		return 0;
	case (u_long)HAKIOS_NFC_STATUS:
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
