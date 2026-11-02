/*
 * /dev/hakios-nfc, exercised from user space: the ST54's power/reset state machine and an APDU transceive, against
 * the modeled chip (nfc=1 nfc_model=1).
 *
 * What this proves is the part of the driver that is the same on the phone: the device node, the ioctls, the power
 * sequencing and its refusals, and that a framed APDU comes back. What it cannot prove is anything about a real
 * ST54 -- QEMU has no NFC controller and the phone's pins are not known (devices/b651/NFC.md).
 *
 * Raw system calls, like userland/src/hello.c: no libc, no dyld, static arm64 Mach-O.
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

static long
syscall1(long num, long a)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16) : "memory", "cc");
	return x0;
}

#define SYS_write           4
#define SYS_read            3
#define SYS_open            5
#define SYS_ioctl           54
#define SYS_semwait_signal  334
#define O_RDWR              2
#define O_RDONLY            0

/* the ABI, from devices/b651/drivers/nfc/hakios.nfc.c; the numbers are <sys/ioccom.h>'s encoding written out */
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

#define EINVAL   22
#define EBUSY    16
#define ENOTSUP  45
#define ENOTTY   25

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
put_hex8(int fd, unsigned v)
{
	const char *d = "0123456789abcdef";

	put(fd, "0x");
	for (int shift = 28; shift >= 0; shift -= 4) {
		char c[2] = { d[(v >> shift) & 0xf], 0 };
		put(fd, c);
	}
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

static int
expect(int out, const char *name, long got, long want)
{
	put(out, name);
	if (got == want) {
		put(out, ": pass\n");
		return 0;
	}
	put(out, ": FAIL errno=");
	put_number(out, got);
	put(out, " (wanted ");
	put_number(out, want);
	put(out, ")\n");
	return 1;
}

static long
ioctl_num(int fd, unsigned long cmd, void *arg, long value)
{
	/* one ioctl with an int in a register-adjacent buffer: the argument is always a pointer to 4 or 16 bytes */
	*(long *)arg = value;
	return syscall3(SYS_ioctl, fd, (long)cmd, (long)arg);
}

void
start(void)
{
	int out = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	int fd;
	long failed = 0, ret;
	struct hakios_nfc_status st;
	unsigned buf[8];

	put(out, "\n--- hakios-nfc: the ST54's state machine and an APDU, from user space ---\n");
	fd = (int)syscall3(SYS_open, (long)"/dev/hakios-nfc", O_RDWR, 0);
	if (fd < 0) {
		put(out, "nfc_test: no /dev/hakios-nfc (boot without nfc=1?); nothing to test\n");
		goto stay;
	}
	put(out, "nfc_test: /dev/hakios-nfc opens as fd ");
	put_number(out, fd);
	put(out, "\n");

	/* 1. the state it starts in */
	zero(&st, sizeof(st));
	failed += expect(out, "status", syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st), 0);
	failed += expect(out, "status.powered at rest", st.powered, 0);
	failed += expect(out, "status.modeled", st.modeled, 1);

	/* 2. an APDU with the chip powered down is refused, not answered from nothing */
	buf[0] = 0x00A40400;
	ret = syscall3(SYS_write, fd, (long)buf, 4);
	failed += expect(out, "write with the chip down", ret, EBUSY);

	/* 3. power on, and the chip answers */
	failed += expect(out, "power on", ioctl_num(fd, HAKIOS_NFC_POWER, &st, 1), 0);
	failed += expect(out, "power on again (idempotent)", ioctl_num(fd, HAKIOS_NFC_POWER, &st, 1), 0);
	zero(&st, sizeof(st));
	failed += expect(out, "status after power on", syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st), 0);
	failed += expect(out, "status.powered", st.powered, 1);

	/* 4. the eSE request */
	failed += expect(out, "use the secure element", ioctl_num(fd, HAKIOS_NFC_USE_ESE, &st, 1), 0);
	zero(&st, sizeof(st));
	syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st);
	failed += expect(out, "status.ese_selected", st.ese_selected, 1);

	/* 5. SELECT by AID: 00 A4 04 00 -> 90 00 (a write, then the read that takes the answer) */
	buf[0] = 0;                                 /* an APDU is bytes: build it as one, not as a packed word */
	((unsigned char *)buf)[0] = 0x00;
	((unsigned char *)buf)[1] = 0xA4;
	((unsigned char *)buf)[2] = 0x04;
	((unsigned char *)buf)[3] = 0x00;
	ret = syscall3(SYS_write, fd, (long)buf, 4);
	failed += expect(out, "write(00 A4 04 00) took the whole frame", ret, 4);
	buf[0] = 0;
	ret = syscall3(SYS_read, fd, (long)buf, 16);
	failed += expect(out, "read() returns the reply length", ret, 2);
	put(out, "nfc_test: SELECT answered ");
	put_hex8(out, ((((unsigned char *)buf)[0] << 8) | ((unsigned char *)buf)[1]));
	put(out, "\n");
	failed += expect(out, "SELECT's status word is 9000", (((unsigned char *)buf)[0] << 8) | ((unsigned char *)buf)[1], 0x9000);

	/* 6. an unknown instruction: 6A 82, the standard "not found" */
	buf[0] = 0;
	((unsigned char *)buf)[0] = 0x00;
	((unsigned char *)buf)[1] = 0xB0;
	((unsigned char *)buf)[2] = 0x00;
	((unsigned char *)buf)[3] = 0x00;
	ret = syscall3(SYS_write, fd, (long)buf, 4);
	failed += expect(out, "write(00 B0 00 00)", ret, 4);
	buf[0] = 0;
	ret = syscall3(SYS_read, fd, (long)buf, 16);
	failed += expect(out, "read() returns 2", ret, 2);
	failed += expect(out, "unknown instruction is 6A82", (((unsigned char *)buf)[0] << 8) | ((unsigned char *)buf)[1], 0x6A82);

	/* 7. reset, and the count moves */
	failed += expect(out, "reset", ioctl_num(fd, HAKIOS_NFC_RESET, &st, 0), 0);
	zero(&st, sizeof(st));
	syscall3(SYS_ioctl, fd, (long)HAKIOS_NFC_STATUS, (long)&st);
	failed += expect(out, "status.reset_count", st.reset_count, 1);
	failed += expect(out, "status.frames (two transceives)", st.frames, 2);

	/* 8. what it refuses */
	failed += expect(out, "unknown ioctl", ioctl_num(fd, _IO('N', 99), &st, 0), ENOTTY);

	/* 9. power off, and the chip stops answering again */
	failed += expect(out, "power off", ioctl_num(fd, HAKIOS_NFC_POWER, &st, 0), 0);
	buf[0] = 0x00A40400;
	failed += expect(out, "write with the chip down again", syscall3(SYS_write, fd, (long)buf, 4), EBUSY);

	put(out, failed ? "nfc_test: FAILED " : "nfc_test: all ");
	put_number(out, failed ? failed : 14);
	put(out, failed ? " check(s)\n" : " checks pass\n");

stay:
	if (sleep_seconds(1) == 0) {
		for (;;) {
			sleep_seconds(60);
		}
	}
	put(out, "nfc_test: no clock here, staying anyway\n");
	for (;;) {
		sleep_seconds(60);
	}
}
