/*
 * /dev/hakios-epd, exercised from user space: the e-ink panel's update/complete cycle, without a panel.
 *
 * Runs in the simulator with `epd=1 epd_model=1`. It submits updates through the driver's own ABI and waits on the
 * markers, so what it proves is the part of the driver that is the same on the phone: the rect check, one update at
 * a time, EBUSY on a marker already in flight, HAKIOS_EPD_WAIT sleeping until HAKIOS_EPD_UPDATE's time is up, and
 * the collision bitmap. It cannot prove anything about the panel -- there is none in QEMU.
 *
 * Raw system calls, like userland/src/hello.c: no libc, no dyld, static arm64 Mach-O. It is loaded as the init
 * program, so process 1 stays resident at the end (exiting panics the kernel).
 *
 *   ioctl(fd, HAKIOS_EPD_UPDATE, &update)   -> 0, or EINVAL / EBUSY / ENOTSUP
 *   ioctl(fd, HAKIOS_EPD_WAIT, &wait)       -> 0 once that marker's update has finished
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
syscall6(long num, long a, long b, long c, long d, long e, long f)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
	    : "memory", "cc");
	return x0;
}

#define SYS_exit            1
#define SYS_write           4
#define SYS_open            5
#define SYS_ioctl           54
#define SYS_sysctlbyname    274
#define SYS_semwait_signal  334
#define O_RDWR              2

/* The ABI, from devices/b651/drivers/display/hakios.epd.c. Kept in step by hand: it is small, and it is the point
 * of the test that both sides agree on it. The ioctl numbers are <sys/ioccom.h>'s own encoding, written out so the
 * arithmetic is visible rather than a magic constant: _IOC(inout, group, num, len)
 *   = inout | ((len & 0x1fff) << 16) | (group << 8) | num,  group 'E' = 0x45 */
struct hakios_epd_update {
	unsigned top, left, width, height;
	unsigned update_mode;           /* 0 partial, 1 full */
	unsigned waveform_mode;         /* 0 default; 4 gc16, 132 gl16, 180 du, 1 a2 */
	unsigned marker;                /* must not be 0 */
	int temperature;                /* INT32_MIN = unset */
};

struct hakios_epd_wait {
	unsigned marker;                /* 0 = any */
	unsigned collision;             /* out */
};

#define IOC_OUT         0x40000000UL
#define IOC_IN          0x80000000UL
#define IOCPARM_MASK    0x1fffUL
#define _IOC(dir, group, num, len)  ((dir) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num))
#define _IOW(g, n, t)               _IOC(IOC_IN, g, n, sizeof(t))
#define _IOWR(g, n, t)              _IOC(IOC_IN | IOC_OUT, g, n, sizeof(t))

#define HAKIOS_EPD_UPDATE      _IOW('E', 1, struct hakios_epd_update)
#define HAKIOS_EPD_WAIT        _IOWR('E', 2, struct hakios_epd_wait)

#define UPDATE_PARTIAL         0
#define UPDATE_FULL            1

#define ENOENT    2
#define EINTR     4
#define EINVAL   22
#define EBUSY    16
#define ENOTSUP  45

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
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(fd, buf + i);
}

/* A relative sleep, so the run does not spin a core while it waits: __semwait_signal(.., timeout=1, relative=1, s, ns) */
static long
sleep_seconds(long seconds)
{
	return syscall6(SYS_semwait_signal, 0, 0, 1, 1, seconds, 0);
}

/* One case, reported as "name: pass" or "name: FAIL errno=N". */
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
submit(int fd, unsigned top, unsigned left, unsigned width, unsigned height, unsigned update_mode,
    unsigned waveform_mode, unsigned marker)
{
	struct hakios_epd_update u;

	u.top = top;
	u.left = left;
	u.width = width;
	u.height = height;
	u.update_mode = update_mode;
	u.waveform_mode = waveform_mode;
	u.marker = marker;
	u.temperature = 0;                                  /* 0 C: "unset" is INT32_MIN, and 0 is a real reading */
	return syscall3(SYS_ioctl, fd, (long)HAKIOS_EPD_UPDATE, (long)&u);
}

static long
wait_for(int fd, unsigned marker, unsigned *collision)
{
	struct hakios_epd_wait w;
	long ret;

	w.marker = marker;
	w.collision = 0xdeadbeef;
	ret = syscall3(SYS_ioctl, fd, (long)HAKIOS_EPD_WAIT, (long)&w);
	*collision = w.collision;
	return ret;
}

/*
 * An update that wants to be accepted, on a panel this program does not have to itself.
 *
 * The console drives the same panel (osfmk/console/video_console.c -> hakios_epd_console_dirty, folded into one
 * update by the driver's thread), and this test's own put() calls are console writes. So a submit can find the
 * console's refresh in flight and get EBUSY. That is the documented answer, and what a caller does about it is
 * retry; before this the test assumed it was the only user and the three submits around the console's first full
 * refresh failed with errno 16, which reads as a driver regression and is not one.
 *
 * wait_for(fd, 0, ...) waits on whatever is in flight, whoever started it: epd_wait() treats marker 0 as "any".
 * Bounded at 200 tries (each a full waveform, so several seconds), which fails the test rather than hanging it if
 * the panel stops completing updates.
 */
static long
submit_retry(int fd, unsigned top, unsigned left, unsigned width, unsigned height, unsigned update_mode,
    unsigned waveform_mode, unsigned marker)
{
	long ret = EBUSY;
	unsigned collision;

	for (int tries = 0; tries < 200 && ret == EBUSY; tries++) {
		(void)wait_for(fd, 0, &collision);
		ret = submit(fd, top, left, width, height, update_mode, waveform_mode, marker);
	}
	return ret;
}

void
start(void)
{
	int out = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	int fd;
	long failed = 0, ret;
	unsigned collision;

	put(out, "\n--- hakios-epd: the panel's update cycle, from user space ---\n");
	fd = (int)syscall3(SYS_open, (long)"/dev/hakios-epd", O_RDWR, 0);
	if (fd < 0) {
		put(out, "epd_test: no /dev/hakios-epd (boot without epd=1?); nothing to test\n");
		goto stay;
	}
	put(out, "epd_test: /dev/hakios-epd opens as fd ");
	put_number(out, fd);
	put(out, "\n");

	/* 1. a well-formed update is accepted, and the wait that follows returns 0 */
	ret = submit_retry(fd, 0, 0, 32, 32, UPDATE_PARTIAL, 1 /* a2: the shortest, 2 frames */, 101);
	failed += expect(out, "update(a2, 32x32)", ret, 0);
	ret = wait_for(fd, 101, &collision);
	failed += expect(out, "wait(101)", ret, 0);
	failed += expect(out, "wait(101) collision", collision, 0);

	/* 2. the same marker cannot be in flight twice: the second submit is EBUSY.
	 *
	 * gc16 is 24 frames (about 400 ms), and the marker is checked immediately after the submit that started it, so
	 * the window in which the panel could have completed it is the few microseconds between two ioctls -- the
	 * console's own refresh cannot shorten that. If this ever does read as accepted, the waveform is not running,
	 * which is a different and worse finding than a flaky test. */
	ret = submit_retry(fd, 0, 0, 32, 32, UPDATE_PARTIAL, 0 /* gc16: 24 frames, long enough to be in flight */, 102);
	failed += expect(out, "update(gc16, 32x32) marker 102", ret, 0);
	ret = submit(fd, 0, 0, 32, 32, UPDATE_PARTIAL, 0, 102);
	failed += expect(out, "update again, marker 102 in flight", ret, EBUSY);
	ret = wait_for(fd, 102, &collision);
	failed += expect(out, "wait(102)", ret, 0);

	/* 3. what the driver refuses before it looks at the controller */
	ret = submit(fd, 0, 0, 0, 32, UPDATE_PARTIAL, 0, 103);
	failed += expect(out, "update 0 width", ret, EINVAL);
	ret = submit(fd, 0, 0, 32, 32, UPDATE_PARTIAL, 0, 0);
	failed += expect(out, "update marker 0", ret, EINVAL);
	ret = submit(fd, 0, 0, 32, 32, 7 /* neither partial nor full */, 0, 104);
	failed += expect(out, "update mode 7", ret, EINVAL);
	ret = submit(fd, 0, 0, 32, 32, UPDATE_PARTIAL, 999 /* no such waveform */, 105);
	failed += expect(out, "update waveform 999", ret, EINVAL);

	/* 4. the wait on a marker that is not in flight returns at once */
	ret = wait_for(fd, 999, &collision);
	failed += expect(out, "wait(999), nothing in flight", ret, 0);

	/* 5. a full refresh of the whole panel, the mode that flashes */
	ret = submit_retry(fd, 0, 0, 1125, 2436, UPDATE_FULL, 4 /* gc16 */, 106);
	failed += expect(out, "update(full, 1125x2436, gc16)", ret, 0);
	ret = wait_for(fd, 106, &collision);
	failed += expect(out, "wait(106)", ret, 0);

	put(out, failed ? "epd_test: FAILED " : "epd_test: all ");
	put_number(out, failed ? failed : 12);
	put(out, failed ? " check(s)\n" : " checks pass\n");

stay:
	/* Process 1 is not supposed to exit; doing so panics the kernel and ends the run. */
	if (sleep_seconds(1) == 0) {
		for (;;) {
			sleep_seconds(60);
		}
	}
	put(out, "epd_test: no clock here, staying anyway\n");
	for (;;) {
		sleep_seconds(60);
	}
}
