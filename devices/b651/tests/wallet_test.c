/*
 * The contactless payment conversation, from user space, against the MODELED ST54 in /dev/hakios-nfc
 * (boot-arg nfc=1 nfc_model=1). This is the sequence a terminal sends a card, in the order it sends it:
 *
 *   power on -> the eSE -> a card in the field   (ioctl HAKIOS_NFC_CARD, MODEL ONLY)
 *     SELECT 2PAY.SYS.DDF01     the PPSE: "which payment applications are here?"
 *     SELECT A0000000031010     the Visa AID that the PPSE just named
 *     GET PROCESSING OPTIONS    "here is the data your PDOL asked for; what do we do now?"
 *
 * What this proves: the device node, the ioctls, and that the modeled chip answers these three APDUs with the exact
 * bytes its templates declare. The comparison is byte for byte against the arrays below, which were built from that
 * TLV, so a driver template that drifts by one length byte fails here rather than passing on a plausible header.
 * The TLV of the reply is not re-walked: those arrays are copies of the driver's own bytes, and a walk of a copy
 * certifies only the copy. A template whose lengths lie is therefore caught by the bytes differing, not by the
 * parse. Also that a wallet with NO card fails in a defined way (6A 82, "file not found") instead of hanging, and
 * that a reset or a removal rolls the application selection back.
 *
 * What it does NOT prove, and must not be read as: anything about a real ST54, a real card, a real token, a real
 * cryptogram or real money. There is no GENERATE AC in this conversation, because the model cannot compute one.
 * devices/b651/WALLET.md is the honest account of the gap.
 *
 * Raw system calls, like userland/src/hello.c: no libc, no dyld, static arm64 Mach-O. Loaded as the init program, so
 * process 1 stays resident at the end (exiting panics the kernel).
 */
typedef unsigned long u64;

static long
syscall3(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2) : "memory", "cc");
	return x0;
}

#define SYS_write           4
#define SYS_read            3
#define SYS_open            5
#define SYS_ioctl           54
#define SYS_semwait_signal  334
#define O_RDWR              2

/* The ABI, from devices/b651/drivers/nfc/hakios.nfc.c. Kept in step by hand: it is small, and both sides agreeing
 * on it is part of what the test is for. The ioctl numbers are <sys/ioccom.h>'s encoding written out. */
struct hakios_nfc_status {
	unsigned powered, reset_count, ese_selected, frames, modeled;
};

#define IOC_OUT         0x40000000UL
#define IOC_IN          0x80000000UL
#define IOCPARM_MASK    0x1fffUL
#define _IOC(dir, group, num, len)  ((dir) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num))
#define _IOW(g, n, t)               _IOC(IOC_IN, g, n, sizeof(t))
#define _IOR(g, n, t)               _IOC(IOC_OUT, g, n, sizeof(t))
#define IOC_VOID        0x20000000UL
#define _IO(g, n)                   _IOC(IOC_VOID, g, n, 0)

#define HAKIOS_NFC_POWER        _IOW('N', 1, int)
#define HAKIOS_NFC_RESET        _IO('N', 2)
#define HAKIOS_NFC_USE_ESE      _IOW('N', 3, unsigned)
#define HAKIOS_NFC_STATUS       _IOR('N', 4, struct hakios_nfc_status)
#define HAKIOS_NFC_CARD         _IOW('N', 5, unsigned)        /* MODEL ONLY */

#define EBUSY    16
#define ENOTSUP  45

/* ---- the APDUs, as bytes ------------------------------------------------------------------------------------ */

/* SELECT "2PAY.SYS.DDF01": 00 A4 04 00 0E <14 bytes of DF name> 00 -- 20 bytes. */
static const unsigned char select_ppse[] = {
	0x00, 0xA4, 0x04, 0x00, 0x0E,
	0x32, 0x50, 0x41, 0x59, 0x2E, 0x53, 0x59, 0x53, 0x2E, 0x44, 0x44, 0x46, 0x30, 0x31,
	0x00
};

/* SELECT A0000000031010 (Visa): 00 A4 04 00 07 <7 bytes of AID> 00 -- 13 bytes. */
static const unsigned char select_visa[] = {
	0x00, 0xA4, 0x04, 0x00, 0x07,
	0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10,
	0x00
};

/* An AID the model does not hold: nothing may select it. */
static const unsigned char select_unknown[] = {
	0x00, 0xA4, 0x04, 0x00, 0x07,
	0xA0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10,
	0x00
};

/*
 * GET PROCESSING OPTIONS, carrying the PDOL data the driver's 9F38 asks for, in its order and its lengths:
 *   TTQ 9F66/4, amount 9F02/6, amount other 9F03/6, country 9F1A/2, TVR 95/5, currency 5F2A/2, date 9A/3,
 *   type 9C/1, unpredictable number 9F37/4  -- 57 bytes, so Lc = 0x3B and the 83 list is 0x39 + 57.
 * 65 bytes. The values are all zero on purpose: the model checks the SHAPE of this data, not its content, because
 * there is no cryptogram to compute from it and a made-up amount would look like a transaction that did not happen.
 */
static const unsigned char get_processing_options[] = {
	0x80, 0xA8, 0x00, 0x00, 0x3B, 0x83, 0x39,
	0x9F, 0x66, 0x04, 0x00, 0x00, 0x00, 0x00,
	0x9F, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9F, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9F, 0x1A, 0x02, 0x00, 0x00,
	0x95, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x5F, 0x2A, 0x02, 0x00, 0x00,
	0x9A, 0x03, 0x00, 0x00, 0x00,
	0x9C, 0x01, 0x00,
	0x9F, 0x37, 0x04, 0x00, 0x00, 0x00, 0x00,
	0x00
};

/* The same command with Lc = 2 and no 83 tag in the data: a terminal that got the PDOL wrong. */
static const unsigned char gpo_malformed[] = {
	0x80, 0xA8, 0x00, 0x00, 0x02, 0x9F, 0x37, 0x00
};

/* ---- the responses the driver's templates declare ------------------------------------------------------------ */

/* PPSE FCI after SELECT: 6F 30, one application (the Visa AID, priority 1), then 90 00. 52 bytes. */
static const unsigned char ppse_fci[] = {
	0x6F, 0x30, 0x84, 0x0E, 0x32, 0x50, 0x41, 0x59, 0x2E, 0x53, 0x59, 0x53,
	0x2E, 0x44, 0x44, 0x46, 0x30, 0x31, 0xA5, 0x1E, 0xBF, 0x0C, 0x1B, 0x61,
	0x19, 0x4F, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0x50, 0x0B,
	0x56, 0x49, 0x53, 0x41, 0x20, 0x43, 0x52, 0x45, 0x44, 0x49, 0x54, 0x87,
	0x01, 0x01, 0x90, 0x00
};

/* Visa FCI after SELECT: 6F 5C with the 9F38 PDOL (57 bytes of tag/length pairs), then 90 00. 96 bytes. */
static const unsigned char visa_fci[] = {
	0x6F, 0x5C, 0x84, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0xA5,
	0x51, 0x50, 0x0B, 0x56, 0x49, 0x53, 0x41, 0x20, 0x43, 0x52, 0x45, 0x44,
	0x49, 0x54, 0x87, 0x01, 0x01, 0x5F, 0x2D, 0x02, 0x65, 0x6E, 0x9F, 0x38,
	0x39, 0x9F, 0x66, 0x04, 0x00, 0x00, 0x00, 0x00, 0x9F, 0x02, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x9F, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x9F, 0x1A, 0x02, 0x00, 0x00, 0x95, 0x05, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x5F, 0x2A, 0x02, 0x00, 0x00, 0x9A, 0x03, 0x00, 0x00, 0x00,
	0x9C, 0x01, 0x00, 0x9F, 0x37, 0x04, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00
};

/* GPO response: 77 0A, AIP 00 00 (no auth method claimed), AFL one entry SFI 1 record 1, then 90 00. */
static const unsigned char gpo_response[] = {
	0x77, 0x0A,
	0x82, 0x02, 0x00, 0x00,
	0x94, 0x04, 0x08, 0x01, 0x01, 0x00,
	0x90, 0x00
};

/* ---- output and checks -------------------------------------------------------------------------------------- */

static unsigned checks;

static unsigned
length(const char *s)
{
	unsigned n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
put(int fd, const char *s)
{
	syscall3(SYS_write, fd, (long)s, length(s));
}

static void
put_number(int fd, long v)
{
	char buf[24];
	int i = sizeof(buf);
	int negative = v < 0;

	if (negative) {
		v = -v;
	}
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	if (negative) {
		buf[--i] = '-';
	}
	put(fd, buf + i);
}

static void
zero(void *p, unsigned n)
{
	unsigned char *b = p;

	while (n--) {
		*b++ = 0;
	}
}

static long
sleep_seconds(long seconds)
{
	register long x16 __asm__("x16") = SYS_semwait_signal;
	register long x0 __asm__("x0") = 0;
	register long x1 __asm__("x1") = 0;
	register long x2 __asm__("x2") = 1;
	register long x3 __asm__("x3") = 1;
	register long x4 __asm__("x4") = seconds;
	register long x5 __asm__("x5") = 0;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
	    : "memory", "cc");
	return x0;
}

/* Every check goes through one of these, so the total printed at the end is counted rather than claimed. */
static int
report(int out, const char *name, int ok, long got, long want)
{
	checks++;
	put(out, name);
	if (ok) {
		put(out, ": pass\n");
		return 0;
	}
	put(out, ": FAIL got ");
	put_number(out, got);
	put(out, " (wanted ");
	put_number(out, want);
	put(out, ")\n");
	return 1;
}

static int
expect(int out, const char *name, long got, long want)
{
	return report(out, name, got == want, got, want);
}

/* A byte-string comparison: the reply must BE these bytes, so the whole TLV is covered and not just its header.
 * This does not parse the TLV -- see the file's header on why a parse of the expected copy would prove nothing
 * extra -- it requires every byte of it, length bytes included, to match. */
static int
expect_bytes(int out, const char *name, const unsigned char *got, long got_len, const unsigned char *want,
    long want_len)
{
	long i;

	checks++;
	put(out, name);
	if (got_len == want_len) {
		for (i = 0; i < want_len && got[i] == want[i]; i++) {
		}
		if (i == want_len) {
			put(out, ": pass\n");
			return 0;
		}
	}
	put(out, ": FAIL (reply is ");
	put_number(out, got_len);
	put(out, " bytes, wanted ");
	put_number(out, want_len);
	put(out, " and equal)\n");
	return 1;
}

/* The status word: the last two bytes of the reply, big-endian. */
static long
status_word(const unsigned char *buf, long len)
{
	if (len < 2) {
		return -1;
	}
	return ((long)buf[len - 2] << 8) | buf[len - 1];
}

static long
ioctl_num(int fd, unsigned long cmd, void *arg, long value)
{
	*(long *)arg = value;
	return syscall3(SYS_ioctl, fd, (long)cmd, (long)arg);
}

/* One APDU: the write, then the read that collects its answer. Returns the reply length, or a negative errno. */
static long
transceive(int fd, const unsigned char *apdu, unsigned apdu_len, unsigned char *reply, long room)
{
	long ret = syscall3(SYS_write, fd, (long)apdu, (long)apdu_len);

	if (ret != (long)apdu_len) {
		return ret < 0 ? ret : -1;      /* a short write is as bad as an error here */
	}
	zero(reply, (unsigned)room);
	return syscall3(SYS_read, fd, (long)reply, room);
}

void
start(void)
{
	int out = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	int fd;
	long failed = 0, ret;
	struct hakios_nfc_status st;
	unsigned arg[8];
	unsigned char reply[128];       /* the largest answer here is the 96-byte FCI */

	put(out, "\n--- hakios-nfc: the contactless payment conversation, against the modeled ST54 ---\n");
	fd = (int)syscall3(SYS_open, (long)"/dev/hakios-nfc", O_RDWR, 0);
	if (fd < 0) {
		put(out, "wallet_test: no /dev/hakios-nfc (boot without nfc=1?); nothing to test\n");
		goto stay;
	}

	/* 1. the chip is the model: HAKIOS_NFC_CARD exists only here */
	zero(&st, sizeof(st));
	failed += expect(out, "status", syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st), 0);
	failed += expect(out, "status.modeled", (long)st.modeled, 1);

	/* 2. power on and select the eSE, as a terminal's host does before the field is up */
	failed += expect(out, "power on", ioctl_num(fd, HAKIOS_NFC_POWER, arg, 1), 0);
	failed += expect(out, "the eSE", ioctl_num(fd, HAKIOS_NFC_USE_ESE, arg, 1), 0);

	/* 3. no card yet. A SELECT must come back as a defined error, and must come back at all: a terminal in the
	 * field with no card gets nothing, but a host reading the device must not be left waiting on a reply. */
	failed += expect(out, "card absent", ioctl_num(fd, HAKIOS_NFC_CARD, arg, 0), 0);
	ret = transceive(fd, select_ppse, sizeof(select_ppse), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT PPSE with no card: reply length", ret, 2);
	failed += expect(out, "SELECT PPSE with no card is 6A82", status_word(reply, ret), 0x6A82);
	ret = transceive(fd, get_processing_options, sizeof(get_processing_options), reply, (long)sizeof(reply));
	failed += expect(out, "GPO with no card: reply length", ret, 2);
	failed += expect(out, "GPO with no card is 6A82", status_word(reply, ret), 0x6A82);

	/* 4. a card in the field */
	failed += expect(out, "card present", ioctl_num(fd, HAKIOS_NFC_CARD, arg, 1), 0);

	/* 5. SELECT the PPSE: the terminal asks which payment applications the card holds */
	ret = transceive(fd, select_ppse, sizeof(select_ppse), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT PPSE: reply length", ret, (long)sizeof(ppse_fci));
	failed += expect(out, "SELECT PPSE: status word", status_word(reply, ret), 0x9000);
	failed += expect_bytes(out, "SELECT PPSE: the FCI is byte for byte", reply, ret, ppse_fci,
	    (long)sizeof(ppse_fci));

	/* 6. SELECT the AID the PPSE named: the application, with the PDOL it wants filled */
	ret = transceive(fd, select_visa, sizeof(select_visa), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT AID: reply length", ret, (long)sizeof(visa_fci));
	failed += expect(out, "SELECT AID: status word", status_word(reply, ret), 0x9000);
	failed += expect_bytes(out, "SELECT AID: the FCI (with its PDOL) is byte for byte", reply, ret, visa_fci,
	    (long)sizeof(visa_fci));

	/* 7. an AID the card does not hold: 6A 82, and the card stays in the field */
	ret = transceive(fd, select_unknown, sizeof(select_unknown), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT an AID the card lacks: 6A82", status_word(reply, ret), 0x6A82);

	/* 8. GET PROCESSING OPTIONS, with the PDOL data filled in: the card says what it will do */
	ret = transceive(fd, get_processing_options, sizeof(get_processing_options), reply, (long)sizeof(reply));
	failed += expect(out, "GPO: reply length", ret, (long)sizeof(gpo_response));
	failed += expect(out, "GPO: status word", status_word(reply, ret), 0x9000);
	failed += expect_bytes(out, "GPO: the response template is byte for byte", reply, ret, gpo_response,
	    (long)sizeof(gpo_response));

	/* 9. a reset drops the selected application: GPO with nothing selected is refused, not answered */
	failed += expect(out, "reset", ioctl_num(fd, HAKIOS_NFC_RESET, arg, 0), 0);
	ret = transceive(fd, get_processing_options, sizeof(get_processing_options), reply, (long)sizeof(reply));
	failed += expect(out, "GPO after reset: 6985 (nothing selected)", status_word(reply, ret), 0x6985);

	/* 10. select the AID again, then send a PDOL that is not an 83 list: 6A 80, told so rather than answered */
	ret = transceive(fd, select_visa, sizeof(select_visa), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT AID after reset: status word", status_word(reply, ret), 0x9000);
	ret = transceive(fd, gpo_malformed, sizeof(gpo_malformed), reply, (long)sizeof(reply));
	failed += expect(out, "GPO with a malformed PDOL: 6A80", status_word(reply, ret), 0x6A80);

	/* 11. the wallet loses its card mid-conversation: the next SELECT finds nothing */
	failed += expect(out, "card removed", ioctl_num(fd, HAKIOS_NFC_CARD, arg, 0), 0);
	ret = transceive(fd, select_visa, sizeof(select_visa), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT AID after removal: 6A82", status_word(reply, ret), 0x6A82);

	/* 12. and a card put back answers from scratch, as a card entering the field does */
	failed += expect(out, "card presented again", ioctl_num(fd, HAKIOS_NFC_CARD, arg, 1), 0);
	ret = transceive(fd, select_ppse, sizeof(select_ppse), reply, (long)sizeof(reply));
	failed += expect(out, "SELECT PPSE after re-presentation: status word", status_word(reply, ret), 0x9000);

	/* 13. what the host can read back */
	zero(&st, sizeof(st));
	failed += expect(out, "status after the conversation",
	    syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st), 0);
	failed += expect(out, "status.powered", (long)st.powered, 1);
	failed += expect(out, "status.ese_selected", (long)st.ese_selected, 1);
	failed += expect(out, "status.frames (eleven transceives)", (long)st.frames, 11);

	/* 14. power off: the chip stops answering, which ends every conversation */
	failed += expect(out, "power off", ioctl_num(fd, HAKIOS_NFC_POWER, arg, 0), 0);
	failed += expect(out, "write with the chip down", syscall3(SYS_write, fd, (long)select_ppse,
	    (long)sizeof(select_ppse)), EBUSY);

	put(out, failed ? "wallet_test: FAILED " : "wallet_test: all ");
	put_number(out, failed ? failed : (long)checks);
	put(out, failed ? " check(s)\n" : " checks pass\n");

stay:
	if (sleep_seconds(1) == 0) {
		for (;;) {
			sleep_seconds(60);
		}
	}
	put(out, "wallet_test: no clock here, staying anyway\n");
	for (;;) {
		sleep_seconds(60);
	}
}
