/*
 * Draw a picture into /dev/hakios-fb, from user space, with no GPU and no library.
 *
 * This is the smallest thing that answers "can a program put pixels on the screen": it asks the panel for its
 * geometry, draws into its own buffer on the processor (a gradient, three discs, a border), and blits it in. With
 * virtio_gpu=1 the device then scanouts that framebuffer, so what it draws is what the host's screen shows.
 *
 * It does not rasterise with a GPU -- nothing in this simulator can (devices/b651/SIM.md). What it does prove is the
 * path: user memory -> /dev/hakios-fb -> the panel -> the display device.
 *
 * Raw system calls, static arm64 Mach-O, no libc (userland/src/hello.c has the same shape).
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
#define SYS_open            5
#define SYS_ioctl           54
#define SYS_semwait_signal  334
#define O_RDWR              2

struct hakios_fb_info {
	unsigned width, height, row_bytes, bpp, format, size;
};

struct hakios_fb_blit {
	unsigned x, y, width, height, row_bytes;
};

#define IOC_OUT         0x40000000UL
#define IOC_IN          0x80000000UL
#define IOC_VOID        0x20000000UL
#define IOCPARM_MASK    0x1fffUL
#define _IOC(dir, g, n, len)  ((dir) | (((len) & IOCPARM_MASK) << 16) | ((g) << 8) | (n))
#define _IOW(g, n, t)         _IOC(IOC_IN, g, n, sizeof(t))
#define _IOR(g, n, t)         _IOC(IOC_OUT, g, n, sizeof(t))
#define _IO(g, n)             _IOC(IOC_VOID, g, n, 0)

#define HAKIOS_FB_INFO   _IOR('F', 1, struct hakios_fb_info)
#define HAKIOS_FB_BLIT   _IOW('F', 2, struct hakios_fb_blit)
#define HAKIOS_FB_CLEAR  _IO('F', 3)

#define W 1125
#define H 2436
#define MAX_PIXELS (W * H)

static unsigned pixels[MAX_PIXELS];        /* a frame: the panel's size, 32 bits a pixel (B,G,R,X) */
static struct hakios_fb_info info;
static struct hakios_fb_blit blit;

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

static long
sleep_seconds(long seconds)
{
	register long x16 __asm__("x16") = SYS_semwait_signal;
	register long x0 __asm__("x0") = 0;
	register long x1 __asm__("x1") = 0;
	register long x2 __asm__("x2") = 2;
	register long x3 __asm__("x3") = 1;
	register long x4 __asm__("x4") = seconds;
	register long x5 __asm__("x5") = 0;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
	    : "memory", "cc");
	return x0;
}

/* B, G, R, X in increasing address: the framebuffer's own order, not 0x00RRGGBB */
static unsigned
bgra(unsigned char r, unsigned char g, unsigned char b)
{
	return (unsigned)b | ((unsigned)g << 8) | ((unsigned)r << 16) | (0xffu << 24);
}

static void
disc(int cx, int cy, int radius, unsigned colour)
{
	int r2 = radius * radius;

	for (int y = cy - radius; y <= cy + radius; y++) {
		if (y < 0 || y >= (int)info.height) {
			continue;
		}
		for (int x = cx - radius; x <= cx + radius; x++) {
			if (x < 0 || x >= (int)info.width) {
				continue;
			}
			int dx = x - cx, dy = y - cy;
			if (dx * dx + dy * dy <= r2) {
				pixels[y * (info.row_bytes / 4) + x] = colour;
			}
		}
	}
}

void
start(void)
{
	int out = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);
	int fd;
	long ret;
	unsigned stride;

	put(out, "\n--- hakios-fb: a picture drawn by a program, no GPU ---\n");
	fd = (int)syscall3(SYS_open, (long)"/dev/hakios-fb", O_RDWR, 0);
	if (fd < 0) {
		put(out, "fb_paint: no /dev/hakios-fb (boot without fbdev=1?)\n");
		goto stay;
	}
	ret = syscall3(SYS_ioctl, fd, (long)HAKIOS_FB_INFO, (long)&info);
	if (ret != 0) {
		put(out, "fb_paint: HAKIOS_FB_INFO failed ");
		put_number(out, ret);
		put(out, "\n");
		goto stay;
	}
	put(out, "fb_paint: the panel is ");
	put_number(out, info.width);
	put(out, "x");
	put_number(out, info.height);
	put(out, ", ");
	put_number(out, info.row_bytes);
	put(out, " bytes a row, ");
	put_number(out, info.bpp);
	put(out, " bits a pixel\n");

	if (info.width > W || info.height > H) {
		put(out, "fb_paint: the panel is bigger than this program's buffer; not drawing\n");
		goto stay;
	}

	/* A picture: a vertical gradient across the panel, three discs, and a one-pixel border. Every pixel is ours, so
	 * the border being drawn last is a check the whole frame reached the panel, not a corner of it. */
	stride = info.row_bytes / 4;
	for (unsigned y = 0; y < info.height; y++) {
		unsigned char g = (unsigned char)(y * 255 / (info.height ? info.height : 1));

		for (unsigned x = 0; x < info.width; x++) {
			unsigned char b = (x < info.width / 3) ? 90 : ((x < 2 * info.width / 3) ? 30 : 0);

			pixels[y * stride + x] = bgra(200, g, b);
		}
	}
	disc((int)info.width / 4, (int)info.height / 3, (int)info.width / 6, bgra(255, 60, 60));
	disc((int)info.width / 2, (int)info.height / 2, (int)info.width / 6, bgra(60, 255, 60));
	disc((int)info.width * 3 / 4, (int)info.height * 2 / 3, (int)info.width / 6, bgra(60, 60, 255));
	for (unsigned x = 0; x < info.width; x++) {
		pixels[x] = bgra(255, 255, 255);
		pixels[(info.height - 1) * stride + x] = bgra(255, 255, 255);
	}
	for (unsigned y = 0; y < info.height; y++) {
		pixels[y * stride] = bgra(255, 255, 255);
		pixels[y * stride + info.width - 1] = bgra(255, 255, 255);
	}

	blit.x = 0;
	blit.y = 0;
	blit.width = info.width;
	blit.height = info.height;
	blit.row_bytes = info.row_bytes;
	ret = syscall3(SYS_ioctl, fd, (long)HAKIOS_FB_BLIT, (long)&blit);
	if (ret != 0) {
		put(out, "fb_paint: HAKIOS_FB_BLIT failed ");
		put_number(out, ret);
		put(out, "\n");
		goto stay;
	}
	/* and the pixels themselves: the rect is set, so this write() is that rect's rows */
	{
		long n = (long)info.row_bytes * info.height;

		put(out, "fb_paint: blit set for ");
		put_number(out, (long)info.width * info.height);
		put(out, " pixels; writing ");
		put_number(out, n);
		put(out, " bytes\n");
		ret = syscall3(SYS_write, fd, (long)pixels, n);
		put(out, "fb_paint: write() took ");
		put_number(out, ret);
		put(out, " bytes\n");
		if (ret != n) {
			put(out, "fb_paint: not the whole frame\n");
			goto stay;
		}
	}
	put(out, "fb_paint: the picture is on the panel\n");

stay:
	if (sleep_seconds(1) == 0) {
		for (;;) {
			sleep_seconds(60);
		}
	}
	put(out, "fb_paint: no clock here, staying anyway\n");
	for (;;) {
		sleep_seconds(60);
	}
}
