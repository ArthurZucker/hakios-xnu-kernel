/*
 * /dev/hakios-fb: the panel, writable from user space. Only with boot-arg fbdev=1.
 *
 * WHY THIS EXISTS. Until now the only thing that could put pixels on the screen was the kernel: the text console
 * draws into the boot framebuffer, and the display shim (MiniIOMobileFramebuffer) copies a window server's surface
 * into it. With virtio_gpu=1 that framebuffer is what QEMU's GPU device puts on the host's screen -- but no program
 * of ours could reach it. This is that door, and it is what makes a rendered picture visible at all in this
 * simulator: a program draws into its own memory and writes it here.
 *
 * WHAT IT IS NOT: it is not a GPU. Nothing here rasterises. A program that wants a picture draws it on the
 * processor and writes the result; the boot argument "virtio_gpu" is what puts that picture on the screen (a 2D
 * scanout). A 3D device can be present -- qemu/virgl-macos.md has one running -- but this path never touches it:
 * the pixels arrive here already drawn.
 *
 * THE FORMAT is the framebuffer's own, as the boot loader left it: 32 bits a pixel, B, G, R, X in increasing
 * address -- the layout virtio-gpu calls B8G8R8A8_UNORM and what MiniIOMobileFramebuffer writes. A caller that
 * wants RGB has to swap the two bytes at its end, and ioctl HAKIOS_FB_INFO tells it the geometry and row pitch so it
 * can. The pitch is the framebuffer's own and is not the width times four: it is what the loader set.
 *
 * A write(2) is a run of pixels, not a picture: the caller writes one row's worth of bytes over and over, or uses
 * HAKIOS_FB_BLIT. Both are the same copy.
 *
 *   HAKIOS_FB_INFO    _IOR('F', 1, struct hakios_fb_info)
 *   HAKIOS_FB_BLIT    _IOW('F', 2, struct hakios_fb_blit)  then that many bytes at the same fd
 *   write()           pixels at the cursor, wrapping at the end of the panel; seek() moves the cursor
 *   ioctl(HAKIOS_FB_CLEAR)
 */
#include <stdint.h>
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

#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer: what the console draws into, and now any caller */

struct hakios_fb_info {
	uint32_t width;                 /* pixels */
	uint32_t height;
	uint32_t row_bytes;             /* the framebuffer's pitch: not width * 4 */
	uint32_t bpp;                   /* bits a pixel, 32 */
	uint32_t format;                /* 0: B,G,R,X in increasing address (B8G8R8A8) */
	uint32_t size;                  /* bytes the panel occupies */
};

struct hakios_fb_blit {
	uint32_t x, y;                  /* where on the panel */
	uint32_t width, height;         /* how many pixels */
	uint32_t row_bytes;             /* the caller's own pitch; 0 = width * 4 */
};

/* The command numbers carry the size of the argument, and the userland caller builds them from its own copy of these
 * two structs (devices/b651/tests/fb_paint.c). If either struct grows a member, its command number changes on one side
 * and not the other, and the ioctl stops matching. The assertions make that a build error here instead of a debug
 * session on the simulator. */
_Static_assert(sizeof(struct hakios_fb_info) == 24, "HAKIOS_FB_INFO's command encodes this size");
_Static_assert(sizeof(struct hakios_fb_blit) == 20, "HAKIOS_FB_BLIT's command encodes this size");

/* Cast to u_long, which is what a cdevsw's d_ioctl is given. <sys/ioccom.h> makes these a uint32_t, and a case
 * label of a 32-bit unsigned value is converted to the switch's promoted type as a *signed* int here: 0x80144602
 * becomes 0xffffffff80144602 and never matches the command. That cost a boot to find; the cast is the fix. */
#define HAKIOS_FB_INFO          ((u_long)_IOR('F', 1, struct hakios_fb_info))
#define HAKIOS_FB_BLIT          ((u_long)_IOW('F', 2, struct hakios_fb_blit))
#define HAKIOS_FB_CLEAR         ((u_long)_IO('F', 3))

/* The largest frame a single blit may carry: a whole panel of 32-bit pixels at 1125x2436 is 10.9 MB, and drawing a
 * frame in one call is the case that matters, so the limit is the panel's own size plus room for a different pitch. */
#define FB_BLIT_MAX             (16u * 1024 * 1024)

static LCK_GRP_DECLARE(fb_lck_grp, "hakios-fb");
static LCK_MTX_DECLARE(fb_lock, &fb_lck_grp);

static uint8_t *fb_base;                /* vinfo.v_baseaddr, cached at init */
static uint32_t fb_width, fb_height, fb_row_bytes;
static size_t fb_cursor;                /* the byte offset write() puts the next pixel at */
static uint64_t fb_writes;
static bool fb_attached;
static struct hakios_fb_blit fb_blit_pending;   /* the rect HAKIOS_FB_BLIT set, if one is waiting for its pixels */
static bool fb_blit_waiting;

static void
fb_geometry(void)
{
	fb_base = (uint8_t *)(uintptr_t)vinfo.v_baseaddr;
	fb_width = vinfo.v_width;
	fb_height = vinfo.v_height;
	fb_row_bytes = vinfo.v_rowbytes;
}

/* Rows of `width` pixels, from `src` (pitch `src_pitch`) to the panel at (x, y).
 *
 * The destination advances by fb_row_bytes, the panel's pitch, and only that is right: the panel is 1125 wide and a
 * row is 4500 bytes, which is 4 bytes a pixel exactly -- but the loader is free to pad, so the row and the picture's
 * width are two different numbers and every destination row step here goes through the pitch, never through width * 4.
 * The same holds for the source: src_pitch is the caller's own and a caller may hand over a padded frame.
 *
 * The rect is validated against the panel by HAKIOS_FB_BLIT and never reaches here out of bounds. This still clips,
 * as a guard for the one window that exists between the ioctl and the write that fills it: if the framebuffer
 * changed size in between, the request is trimmed to the panel instead of writing past it. The comparisons are done
 * in size_t because x + width in 32 bits would wrap for a large width and clip to nothing. */
static void
fb_copy(const uint8_t *src, uint32_t src_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t rows)
{
	if (x >= fb_width || y >= fb_height) {
		return;
	}
	if ((size_t)x + width > fb_width) {
		width = fb_width - x;
	}
	if ((size_t)y + rows > fb_height) {
		rows = fb_height - y;
	}
	for (uint32_t row = 0; row < rows; row++) {
		memcpy(fb_base + (size_t)(y + row) * fb_row_bytes + (size_t)x * 4, src + (size_t)row * src_pitch,
		    (size_t)width * 4);
	}
}

/* Say no, at the ioctl, to a rect that is not on the panel. Clipping it instead would put a frame somewhere other
 * than where the caller asked -- a frame a dozen rows too tall would silently lose its bottom -- and the caller would
 * then write rows this side never consumed. So the edges are refused, and the caller is told which one.
 *
 * `pitch` is the caller's own row and is included because the pixels arrive at that stride: a pitch smaller than a
 * row of pixels, or a frame whose bytes do not fit in one blit, is a request that cannot be carried out. The product
 * is formed in size_t so that a large height cannot wrap a 32-bit multiply into something small and pass. */
static int
fb_rect_bad(const struct hakios_fb_blit *b, uint32_t pitch)
{
	if (b->width == 0 || b->height == 0 || pitch < (size_t)b->width * 4) {
		return EINVAL;
	}
	/* The frame must fit in one blit, and the product is formed in size_t so that a large height cannot wrap a
	 * 32-bit multiply into something small and pass. */
	if ((size_t)pitch * b->height > FB_BLIT_MAX) {
		return EINVAL;
	}
	/* x >= fb_width covers a zero-width rect at the right edge as well as one past it, and leaves the subtraction
	 * below well-defined. */
	if (b->x >= fb_width || b->y >= fb_height || (size_t)b->x + b->width > fb_width ||
	    (size_t)b->y + b->height > fb_height) {
		return ERANGE;
	}
	return 0;
}

/* `len` bytes of pixels from the cursor onwards, row by row. Called with fb_lock held.
 *
 * The cursor is a byte offset into the panel, and it advances through the panel's pitch: what fits on this row, then
 * onto the next, wrapping at the end of the panel, so a caller writing rows hears nothing special and a caller writing
 * a frame row by row is one memcpy a row. */
static void
fb_stream(const uint8_t *src, size_t len)
{
	for (size_t done = 0; done < len;) {
		size_t row = fb_cursor / fb_row_bytes;
		size_t col = fb_cursor % fb_row_bytes;
		size_t room = (size_t)fb_row_bytes - col;
		size_t n = len - done;

		if (n > room) {
			n = room;
		}
		if (row >= fb_height) {
			fb_cursor = 0;          /* the panel is a ring for a stream of pixels */
			continue;
		}
		memcpy(fb_base + row * fb_row_bytes + col, src + done, n);
		fb_cursor += n;
		done += n;
	}
}

static int
fb_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t *chunk;
	size_t size = (size_t)uio_resid(uio);
	user_ssize_t sent = uio_resid(uio);
	int error = 0;

	if (!fb_attached) {
		return ENXIO;
	}
	/* one chunk at a time, and the chunk is as large as a whole frame, so a frame arrives in one memcpy per row
	 * here rather than one system call per row */
	if (size == 0) {
		return 0;
	}
	if (size > FB_BLIT_MAX) {
		size = FB_BLIT_MAX;
	}
	chunk = (uint8_t *)kalloc_data(size, Z_WAITOK);
	if (chunk == NULL) {
		return ENOMEM;
	}
	while (uio_resid(uio) > 0 && error == 0) {
		size_t take = (size_t)uio_resid(uio);

		if (take > size) {
			take = size;
		}
		error = uiomove((caddr_t)chunk, (int)take, uio);
		if (error != 0) {
			break;
		}
		lck_mtx_lock(&fb_lock);
		if (fb_blit_waiting) {
			/* HAKIOS_FB_BLIT named a rect and this write() is its pixels: rows straight from the caller's pitch
			 * into that rect on the panel. The rect takes exactly its own pitch times its height and nothing
			 * else is read as pixels -- the caller may send a longer write, and the bytes past the rect go on
			 * as the ordinary stream, which is what this device did before the rect was recorded.
			 *
			 * A short write is not a frame: half a picture on the panel is not what was asked for, so nothing
			 * is copied unless the whole rect arrives in one go, and the caller's write() returns short of its
			 * request and can retry. */
			size_t want = (size_t)fb_blit_pending.row_bytes * fb_blit_pending.height;

			if (want <= take) {
				fb_copy(chunk, fb_blit_pending.row_bytes, fb_blit_pending.x, fb_blit_pending.y,
				    fb_blit_pending.width, fb_blit_pending.height);
				fb_blit_waiting = false;
				fb_writes++;
				fb_stream(chunk + want, take - want);
				lck_mtx_unlock(&fb_lock);
				continue;
			}
			lck_mtx_unlock(&fb_lock);
			break;
		}
		fb_stream(chunk, take);
		fb_writes++;
		lck_mtx_unlock(&fb_lock);
	}
	kfree_data(chunk, size);
	/* Bytes actually consumed, not bytes requested: a write that stopped because a rect was not filled is a short
	 * write, which is how write(2) tells a caller the rest did not go in. */
	return error == 0 ? (int)(sent - uio_resid(uio)) : error;
}

/* Reading the panel back: the pixels as they are, from the start. A screen grab is what this is for, and the whole
 * panel is small enough to hand over in one go, so there is no cursor to keep (write() has its own). */
static int
fb_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	size_t want = (size_t)uio_resid(uio);
	size_t size = (size_t)fb_row_bytes * fb_height;
	int error;

	if (!fb_attached) {
		return ENXIO;
	}
	if (want > size) {
		want = size;
	}
	lck_mtx_lock(&fb_lock);
	error = uiomove((caddr_t)fb_base, (int)want, uio);
	lck_mtx_unlock(&fb_lock);
	return error;
}

static int
fb_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
	switch (cmd) {
	case (u_long)HAKIOS_FB_INFO:
		{
			struct hakios_fb_info *info = (struct hakios_fb_info *)data;

			info->width = fb_width;
			info->height = fb_height;
			info->row_bytes = fb_row_bytes;
			info->bpp = 32;
			info->format = 0;
			info->size = fb_row_bytes * fb_height;
			return 0;
		}
	case (u_long)HAKIOS_FB_BLIT:
		{
			/* An ioctl's argument is at most IOCPARM_MAX (8 KiB), so the pixels cannot ride along with the
			 * rect. The rect is recorded here and the next write() fills it: "this destination, then these
			 * pixels", in that order, on the same descriptor.
			 *
			 * The caller's struct is 20 bytes and the command says so (_IOW encodes its size), so XNU hands over
			 * a buffer of exactly that size -- a user struct that is smaller or is not this struct cannot reach
			 * here. Until XNU has filled the buffer there is nothing to check: the rectangle and its pitch are
			 * all this can judge, and fb_rect_bad does. */
			const struct hakios_fb_blit *b = (const struct hakios_fb_blit *)data;
			uint32_t pitch = b->row_bytes ? b->row_bytes : b->width * 4;
			int bad = fb_rect_bad(b, pitch);

			if (bad != 0) {
				return bad;
			}
			lck_mtx_lock(&fb_lock);
			fb_blit_pending = *b;
			fb_blit_pending.row_bytes = pitch;
			fb_blit_waiting = true;
			lck_mtx_unlock(&fb_lock);
			return 0;
		}
	case (u_long)HAKIOS_FB_CLEAR:
		lck_mtx_lock(&fb_lock);
		memset(fb_base, 0, (size_t)fb_row_bytes * fb_height);
		fb_cursor = 0;
		fb_writes++;
		lck_mtx_unlock(&fb_lock);
		return 0;
	default:
		printf("hakios-fb: unknown command %#lx (BLIT is %#lx, INFO %#lx)\n", (unsigned long)cmd,
		    (unsigned long)HAKIOS_FB_BLIT, (unsigned long)HAKIOS_FB_INFO);
		return ENOTTY;
	}
}

/* the open file's offset is the cursor: lseek moves it, and marks where write() puts the next pixels.
 *
 * A rect armed by HAKIOS_FB_BLIT is dropped on close: it is a promise made on one descriptor, and letting it outlive
 * that descriptor would put the next program's first write() into a strange place on the panel. fb_blit_waiting is
 * this device's caller state -- the boot framebuffer is behind it -- so the reset here is the pair open()/close()
 * keeping it its own. */
static int
fb_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return fb_attached ? 0 : ENXIO;
}

static int
fb_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(&fb_lock);
	fb_blit_waiting = false;
	lck_mtx_unlock(&fb_lock);
	return 0;
}

static const struct cdevsw fb_cdevsw = {
	.d_open = fb_open, .d_close = fb_close, .d_read = fb_read, .d_write = fb_write,
	.d_ioctl = fb_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_fb_init(void)
{
	uint32_t enabled = 0;
	int major;

	if (!PE_parse_boot_argn("fbdev", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	fb_geometry();
	if (fb_base == NULL || fb_width == 0 || fb_height == 0 || vinfo.v_depth != 32) {
		printf("hakios-fb: no usable boot framebuffer (%ux%u, depth %u)\n", fb_width, fb_height, vinfo.v_depth);
		return;
	}
	major = cdevsw_add(-1, &fb_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-fb") == NULL) {
		printf("hakios-fb: cannot create the device\n");
		return;
	}
	fb_attached = true;
	printf("hakios-fb: /dev/hakios-fb, %ux%u at %u bytes a row, 32 bits a pixel (B,G,R,X)\n", fb_width, fb_height,
	    fb_row_bytes);
}
