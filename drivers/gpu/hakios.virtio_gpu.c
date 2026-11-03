/*
 * QEMU's virtio GPU as a display: the port's first display path that is not a memcpy into the boot framebuffer.
 *
 * "-device virtio-gpu-device" on the virt machine's virtio-mmio bus, with that bus in its current (not
 * "legacy") form: -global virtio-mmio.force-legacy=false. Only with boot-arg virtio_gpu=1, because the bus
 * address is QEMU's and means nothing on the phone.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT. This is the 2D command set the device has always had (VIRTIO_GPU_CMD_*):
 * make a resource of the panel's size, attach it to the scanout, then hand the device a copy of the picture and
 * tell it to flush it. That is a real GPU device doing the scanout -- what was there before was the processor
 * writing pixels into the loader's framebuffer and a model guessing what a panel would do with them.
 *
 * It is NOT 3D. This driver speaks the 2D command set only, and does not send a single VIRTIO_GPU_CMD_CTX_*.
 * The machine can have 3D -- `virtio-gpu-gl-device` works when QEMU is built with virglrenderer over an EGL
 * backend (qemu/virgl-macos.md says how), and then a guest GL driver has a device to talk to -- but this file
 * still talks 2D, so nothing this kernel does makes an OpenGL context run. Saying otherwise would be the one
 * thing this port must not do.
 *
 * The picture comes from the kernel's own console (vinfo), which is what the boot loader left and what the text
 * console draws into: the same memory MiniIOMobileFramebuffer uses, so this device shows the same screen the
 * panel model shows. When the window server draws (fGraphics), what it draws lands there too.
 *
 * THE DAMAGE RECT. The panel this ends up on is e-ink, and an e-ink panel does not take a frame -- it takes "this
 * rect has changed, drive ink over it" (devices/b651/EPDC.md:8-38, :104-107). So the transfer is no longer always
 * the whole screen. The framebuffer is compared against a copy of what the device was last given, a band of rows at
 * a time, and only the span of the bands that differ is transferred and flushed. The same rect is published on
 * /dev/hakios-gpu (HAKIOS_VIRTIO_GPU_DAMAGE) in the top/left/width/height order of hakios_epd_update
 * (devices/b651/drivers/display/hakios.epd.c, `struct hakios_epd_update`), so the panel side can hand it straight
 * to an update. Both the comparison and the rect are coarse, and gpu_damage_span() says what that costs and what it
 * can miss. With nothing to compare against -- the first upload, a framebuffer whose pitch is not the resource's
 * stride, no memory for the shadow -- the whole panel goes, exactly as it did before any of this.
 */
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer: what the text console draws into */

/* ---- the damage rect, for the panel side: /dev/hakios-gpu ------------------------------------------------
 * An e-ink panel is told "this rect changed" and drives ink over it; it is not given a frame to scan out
 * (devices/b651/EPDC.md:8-38, :104-107). So the rect this driver transfers is published here, and the first four
 * fields are hakios_epd_update's own, in its order and its units -- top, left, width, height in pixels from the
 * top-left corner (devices/b651/drivers/display/hakios.epd.c, `struct hakios_epd_update`). A caller can copy them
 * straight into an update: no translation, and nothing to get backwards. That file is another driver's and may
 * change under this one; if it ever reorders those fields, this struct is what has to follow it.
 *
 * width == 0 && height == 0 means nothing has changed since the last time this was read. Reading CLEARS the rect:
 * the union of every transfer since the last read, so a caller that is slower than the 1 Hz transfer sees a bigger
 * rect rather than a missed change (gpu_damage_add() below says why that direction is the safe one).
 *
 * The command number is ours, group 'G' -- no other ioctl of the kernel this is built against uses that letter --
 * and is computed from the struct by <sys/ioccom.h>, not written as hex. It is _IOR and not _IOWR because there is
 * no argument: the rect is the kernel's answer, and nothing the caller passes in means anything.
 *
 * It is not in sys/hakios.virtio_gpu.h, where the driver's kernel-side interface lives, for the reason
 * hakios.epd.c keeps its own ABI in the driver (its "the ABI" block says so): a device's repository may add no file
 * to xnu/files/, and that header is one. When a caller in the tree needs it, it moves there.
 */
struct hakios_gpu_damage {
	uint32_t top;                   /* pixels from the panel's top edge */
	uint32_t left;                  /* pixels from its left edge */
	uint32_t width;                 /* pixels */
	uint32_t height;
	uint32_t transfers;             /* how many transfers this rect is the union of; 0 = none, see above */
};
_Static_assert(sizeof(struct hakios_gpu_damage) == 20, "HAKIOS_VIRTIO_GPU_DAMAGE's command encodes this size");

#define HAKIOS_VIRTIO_GPU_DAMAGE        ((u_long)_IOR('G', 1, struct hakios_gpu_damage))

#define VIRTIO_BUS              0x0a000000UL    /* QEMU virt: 32 transports of 0x200 bytes */
#define VIRTIO_SLOTS            32
#define VIRTIO_SLOT_SIZE        0x200

#define VIRTIO_MAGIC            0x000
#define VIRTIO_VERSION          0x004
#define VIRTIO_DEVICE_ID        0x008
#define VIRTIO_DEVICE_FEATURES  0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES  0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL        0x030
#define VIRTIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_QUEUE_NUM        0x038
#define VIRTIO_QUEUE_READY      0x044
#define VIRTIO_QUEUE_NOTIFY     0x050
#define VIRTIO_STATUS           0x070
#define VIRTIO_QUEUE_DESC       0x080
#define VIRTIO_QUEUE_DRIVER     0x090
#define VIRTIO_QUEUE_DEVICE     0x0a0
#define VIRTIO_CONFIG           0x100
#define VIRTIO_GPU_EVENTS       0x104           /* config: the display's events follow the device's own space */

#define VIRTIO_ID_GPU           16

#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_FEATURES_OK      8
#define STATUS_DRIVER_OK        4

#define QUEUE_SIZE              16
#define QUEUE_CONTROL           0        /* the two ends of a command */
#define DESC_F_NEXT             1
#define DESC_F_WRITE            2

/* ---- the device's own protocol: include/uapi/linux/virtio_gpu.h, the stable part of it ---- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO    0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF      0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT         0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH      0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_CTX_CREATE          0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY         0x0201
#define VIRTIO_GPU_RESP_OK_NODATA          0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO    0x1101

struct virtio_gpu_ctrl_hdr {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint32_t padding;
};
struct virtio_gpu_rect {
	uint32_t x, y, width, height;
};
/* the 3D commands use a box: x, y, z then width, height, depth */
struct virtio_gpu_box {
	uint32_t x, y, z;
	uint32_t w, h, d;
};
struct virtio_gpu_resource_create_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t format;
	uint32_t width;
	uint32_t height;
};
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 2       /* the framebuffer's 32-bit words are B,G,R,unused */
/* device feature bit 0: the device has virgl. This driver never sets it, so no 3D context exists for it. */
#define VIRTIO_GPU_F_VIRGL              1u
struct virtio_gpu_mem_entry {
	uint64_t addr;
	uint32_t length;
	uint32_t padding;
};
struct virtio_gpu_resource_attach_backing {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t nr_entries;
};
struct virtio_gpu_set_scanout {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t scanout_id;
	uint32_t resource_id;
};
struct virtio_gpu_transfer_to_host_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint64_t offset;
	uint32_t resource_id;
	uint32_t padding;
};
struct virtio_gpu_resource_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t resource_id;
	uint32_t padding;
};
struct virtio_gpu_resp_display_info {
	struct virtio_gpu_ctrl_hdr hdr;
	struct {
		struct virtio_gpu_rect r;
		uint32_t enabled;
		uint32_t flags;
	} pmodes[16];
};

/* ---- 3D: the same device, the virgl half of it. include/standard-headers/linux/virtio_gpu.h ---- */
#define GPU_CTX_ID                      1       /* any id but 0; every 3D command names it */
#define VIRTIO_GPU_CAPSET_VIRGL2        2       /* what a virgl2 guest asks for, and what QEMU offers first */

#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D   0x0204
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE  0x0202
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D            0x0207

/* virgl's own command stream, which is what SUBMIT_3D carries. src/virgl_protocol.h upstream. */
#define VIRGL_CCMD_CLEAR_TEXTURE        47      /* the 48th in upstream's enum, counted from VIRGL_CCMD_NOP = 0 */
#define VIRGL_CMD0(cmd, obj, len)       ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

#define VIRGL_FORMAT_B8G8R8A8_UNORM     1
#define VIRGL_BIND_RENDER_TARGET        (1u << 1)
#define PIPE_TEXTURE_2D                 2

struct virtio_gpu_ctx_create {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t nlen;                          /* the debug name's length, at most 64 */
	uint32_t context_init;                  /* VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK == the capset id */
	char debug_name[64];
};

struct virtio_gpu_cmd_submit {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t size;                          /* bytes of command stream that follow this header */
	uint32_t padding;
};

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};

#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800

#define CMD_SIZE                512             /* a command is never longer than this, and a reply either */
#define IO_PAGE                 PAGE_SIZE

static vm_offset_t virtio;
static uint16_t seen;                           /* how far the used ring has been read */
static vm_offset_t ring_mem;                    /* the two rings */
static vm_offset_t buffer_mem;                  /* QUEUE_SIZE buffers of a page: a command and its reply, two halves */
static uint32_t resource_id;
static uint32_t panel_width, panel_height;
static bool attached;
static int refreshes, failures;
static thread_call_t gpu_call;

/*
 * What the device was last given, so the next upload can be the difference rather than the panel.
 *
 * shadow_mem is a plain copy of the framebuffer the last successful transfer read from: the same pitch, the same
 * rows. It is a copy and not the device's own memory because a 2D resource's contents can only be read back with
 * TRANSFER_FROM_HOST_2D -- one more command per frame, and a command is a round trip through the ring, so a
 * comparison that never leaves the guest is the cheaper of the two.
 *
 * The comparison is COARSE and the transfer it produces is coarse with it: rows are compared GPU_BAND_ROWS at a
 * time as one block, so a band with one changed byte marks all of its rows dirty, and the rect that goes to the
 * device is a vertical span of full-width rows rather than a tight box around what changed. That is deliberate.
 * A byte-exact scan of 10.9 MB of framebuffer for one glyph is not worth it on device memory, and the e-ink panel
 * this ends up on cannot do anything finer anyway: a partial update is a rect and a waveform, and the waveform's
 * cost is dominated by the rows it has to drive (devices/b651/EPDC.md:8-38, :104-107). What it costs is that a
 * screen change in one row refreshes up to GPU_BAND_ROWS rows around it. The comparison itself, and what each
 * memcmp is worth, is under "the coarse comparison" below.
 *
 * shadow_valid is false until a transfer has actually succeeded, and stays false whenever the framebuffer's pitch
 * is not the resource's stride -- then the rows the device reads are not the rows this would compare, and the only
 * honest upload is the whole panel (gpu_damage_span() says why).
 */
static uint8_t *shadow_mem;
static bool shadow_valid;

/*
 * The damage rect as it is published on /dev/hakios-gpu: the union of every rect transferred since the last time
 * somebody read it. Held as x0,y0,x1,y1 with x1,y1 exclusive, which is the form a union is cheap in; converted to
 * top/left/width/height once, on the way out.
 *
 * It accumulates and is cleared only by a read, deliberately: a reader slower than the 1 Hz transfer then sees a
 * larger rect (a redraw of more than changed) instead of a smaller one (a change nobody will ever draw). Wrong in
 * the direction that costs ink, never in the direction that loses a pixel.
 */
static uint32_t damage_left, damage_top, damage_right, damage_bottom;
static uint32_t damage_transfers;

/*
 * One lock, and it is only for the rect above. It is NOT held across a device command, on purpose: gpu_command()
 * spins for as long as GPU_CMD_SPINS waiting for the device, which is seconds, and a mutex held that long is a
 * mutex a watchdog is entitled to complain about. The transfers need no lock because there is only ever one of
 * them at a time, by construction: gpu_refresh() runs from the thread call, and the thread call is not entered
 * until hakios_virtio_gpu_init() has finished the 3D path's own refresh. If a second caller is ever added -- a
 * user-space "redraw now", say -- the shadow comparison and the write that follows it stop being atomic and this
 * lock has to grow to cover them, device wait and all.
 */
static LCK_GRP_DECLARE(gpu_lck_grp, "hakios-virtio-gpu");
static LCK_MTX_DECLARE(gpu_lock, &gpu_lck_grp);

static inline uint32_t
reg_read(uint32_t reg)
{
	return *(volatile uint32_t *)(virtio + reg);
}

static inline void
reg_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(virtio + reg) = value;
}

static inline void
reg_write64(uint32_t reg, uint64_t value)
{
	reg_write(reg, (uint32_t)value);
	reg_write(reg + 4, (uint32_t)(value >> 32));
}

/*
 * One command: buffer half 0 holds it, half 1 receives the reply. Returns the reply's type, or 0 for no reply at all.
 * Boot-arg gpu_trace=1: a line per command, because "0" is otherwise ambiguous between "the device refused" and
 * "the device never answered", and those two have nothing to do with each other.
 *
 * The descriptor chain is always the same two, so a used-ring entry whose id is not that chain is a *previous*
 * command finishing late, not this one: it is skipped, not taken. One did exactly that here: SET_SCANOUT's
 * completion landed after the timeout below, and reading it as the reply made a working command look refused.
 */
static uint32_t gpu_trace;

#define GPU_CMD_SPINS   4000000                 /* at tens of microseconds a command this is seconds, not a guess */

static uint32_t
gpu_command(const void *command, uint32_t length)
{
	struct vq_desc *desc = (struct vq_desc *)(ring_mem + OFFSET_DESC);
	struct vq_avail *avail = (struct vq_avail *)(ring_mem + OFFSET_AVAIL);
	struct vq_used *used = (struct vq_used *)(ring_mem + OFFSET_USED);
	struct virtio_gpu_ctrl_hdr *reply = (struct virtio_gpu_ctrl_hdr *)(buffer_mem + IO_PAGE);
	uint32_t type = ((const struct virtio_gpu_ctrl_hdr *)command)->type;

	int spins;

	memcpy((void *)buffer_mem, command, length);
	bzero((void *)(buffer_mem + IO_PAGE), IO_PAGE);
	desc[0].len = length;
	desc[0].flags = DESC_F_NEXT;
	desc[0].next = 1;
	desc[1].len = IO_PAGE;
	desc[1].flags = DESC_F_WRITE;
	desc[1].next = 0;
	avail->ring[avail->idx % QUEUE_SIZE] = 0;
	__builtin_arm_dmb(0xb);
	avail->idx++;
	__builtin_arm_dmb(0xb);
	reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_CONTROL);
	/* the used index is the device's, not ours: the load must happen every time round, or the compiler hoists it
	 * out of the loop and every command looks unanswered */
	for (spins = 0; spins < GPU_CMD_SPINS; spins++) {
		__builtin_arm_dmb(0xb);
		while (seen != *(volatile uint16_t *)&used->idx) {
			uint16_t id = (uint16_t)used->ring[seen % QUEUE_SIZE].id;

			seen++;
			if (id != 0) {
				continue;               /* another chain: not this command's reply */
			}
			if (gpu_trace) {
				printf("gpu: cmd %#x -> reply %#x\n", type, reply->type);
			}

			return reply->type;
		}
	}
	if (gpu_trace) {
		printf("gpu: cmd %#x -> no reply (%u used)\n", type, *(volatile uint16_t *)&used->idx);
	}
	return 0;
}

/*
 * A virgl context: "this guest will send 3D through ctx_id 0". The device answers by asking the host's virgl
 * renderer for a real GL context -- on this QEMU that is a Mesa llvmpipe context over the surfaceless EGL
 * display qemu/virgl-macos.md describes -- so an OK here means a GL context exists on the other end.
 *
 * It does NOT mean a picture. Rendering one needs a resource with a GL texture behind it, and creating that from
 * a guest means a dma-buf (RESOURCE_CREATE_BLOB + PIPE_RESOURCE_SET_TYPE), which this machine has no fd for.
 * So this is the context and the proof that the 3D half of the device is live, not a renderer.
 */
static bool
gpu_ctx_create(void)
{
	struct virtio_gpu_ctx_create ctx = { 0 };
	static const char name[] = "hakios";
	uint32_t type;

	ctx.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
	/* 1, not 0: virgl_renderer_context_create_with_flags refuses ctx_id 0 outright ("user context id must be
	 * greater than 0"), and QEMU ignores that return, so a 0 here fails silently and every 3D command with it. */
	ctx.hdr.ctx_id = GPU_CTX_ID;
	ctx.nlen = sizeof(name) - 1;
	ctx.context_init = VIRTIO_GPU_CAPSET_VIRGL2;
	memcpy(ctx.debug_name, name, sizeof(name) - 1);
	type = gpu_command(&ctx, sizeof(ctx));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: CTX_CREATE (virgl2) refused (%#x); the 3D half of this device stays unused\n", type);
		return false;
	}
	printf("gpu: 3D: a virgl2 context on ctx %d -- the device made a GL context for it on the host\n", GPU_CTX_ID);
	return true;
}

/*
 * A 3D resource over the panel's size. No backing is attached: the texture lives in the host's GL, which is the
 * whole point, and unlike a blob resource this needs no dma-buf fd.
 */
static uint32_t gpu_resource_3d;
static bool gpu_refresh(void);        /* the 2D scanout, defined below */

static bool
gpu_resource_create_3d(uint32_t id)
{
	struct {
		struct virtio_gpu_ctrl_hdr hdr;
		uint32_t resource_id, target, format, bind;
		uint32_t width, height, depth, array_size, last_level, nr_samples, flags, padding;
	} c3d = { 0 };
	uint32_t type;

	c3d.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
	c3d.resource_id = id;
	c3d.target = PIPE_TEXTURE_2D;
	c3d.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
	c3d.bind = VIRGL_BIND_RENDER_TARGET;
	c3d.width = panel_width;
	c3d.height = panel_height;
	c3d.depth = 1;
	c3d.array_size = 1;
	type = gpu_command(&c3d, sizeof(c3d));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: 3D: RESOURCE_CREATE_3D refused (%#x)\n", type);
		return false;
	}
	return true;
}

/*
 * Paint the whole resource on the GPU. The colour rides in the command stream as four floats; upstream's
 * vrend_decode_clear_texture takes them from the command's ARRAY_A..D slots. CLEAR_TEXTURE needs no framebuffer
 * binding, no shaders and no vertex data: it is one glClearTexSubImage, so it is the smallest command that makes
 * the device actually rasterise.
 */
/* 0..1 to a 0..255 byte, rounded rather than truncated: 0.45 * 255 is 114.75 and 115 is the colour asked for */
static uint32_t
epd_byte(float v)
{
	return (uint32_t)(v * 255.0f + 0.5f);
}

static bool
gpu_clear_resource_3d(uint32_t id, float r, float g, float b, float a)
{
	struct {
		struct virtio_gpu_ctrl_hdr hdr;
		uint32_t size;
		uint32_t padding;
		uint32_t cmd;                   /* VIRGL_CCMD_CLEAR_TEXTURE, then its 12 dwords */
		uint32_t handle, level, x, y, z, w, h, d;
		uint32_t colour[4];
	} submit = { 0 };
	uint32_t type;

	submit.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
	submit.hdr.ctx_id = GPU_CTX_ID;
	/* exactly the stream's own bytes: the command word (4) plus the 12 dwords it declares. Deriving this from
	 * sizeof() is wrong -- the struct has padding after the colour and the device parses by this count. */
	submit.size = 4 + 12 * 4;
	submit.cmd = VIRGL_CMD0(VIRGL_CCMD_CLEAR_TEXTURE, 0, 12);
	submit.handle = id;
	submit.w = panel_width;
	submit.h = panel_height;
	submit.d = 1;
	/*
	 * The colour is NOT four floats. vrend hands ARRAY_A..D to glClearTexSubImage as the `data` pointer with the
	 * texture's own format and type -- GL_BGRA / GL_UNSIGNED_BYTE here -- so it reads the first four BYTES as one
	 * pixel. Writing floats there put the bits of 0.1f on the screen as a grey. Pack B, G, R, A into one word.
	 */
	submit.colour[0] = (uint32_t)epd_byte(b) | ((uint32_t)epd_byte(g) << 8) |
	                   ((uint32_t)epd_byte(r) << 16) | ((uint32_t)epd_byte(a) << 24);
	/* gpu_command copies `length` bytes, and the stream must be exactly header + size, so the buffer is
	 * narrowed to that: the struct's trailing padding would otherwise travel as a second, empty command */
	type = gpu_command(&submit, sizeof(submit.hdr) + 8 + submit.size);
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: 3D: CLEAR_TEXTURE refused (%#x)\n", type);
		return false;
	}
	return true;
}

/*
 * The whole 3D path, in the order it has to happen: context, resource, a GPU clear, then scanout. Each step is its
 * own refusal message, because "3D does not work" hides which half of it does not.
 */
static void
gpu_render_3d(void)
{
	gpu_resource_3d = resource_id + 1;
	if (!gpu_resource_3d || !gpu_resource_create_3d(gpu_resource_3d)) {
		return;
	}
	/* the resource has to be in the context before a 3D command can name it: CLEAR_TEXTURE looks it up in the
	 * context, and a SUBMIT_3D that fails inside its stream still replies OK, so the attach has to be here */
	{
		struct {
			struct virtio_gpu_ctrl_hdr hdr;
			uint32_t resource_id;
			uint32_t padding;
		} attach = { 0 };

		attach.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
		attach.hdr.ctx_id = GPU_CTX_ID;
		attach.resource_id = gpu_resource_3d;
		if (gpu_command(&attach, sizeof(attach)) != VIRTIO_GPU_RESP_OK_NODATA) {
			printf("gpu: 3D: CTX_ATTACH_RESOURCE refused (%#x)\n",
			    gpu_command(&attach, sizeof(attach)));
			return;
		}
	}
	if (!gpu_clear_resource_3d(gpu_resource_3d, 0.10f, 0.45f, 0.85f, 1.0f)) {
		return;
	}
	/*
	 * The render has to reach memory the SCREEN is read from, and on this host the display backend that blits
	 * and the virgl renderer are separate GL contexts -- a 3D scanout binds a texture only the display's context
	 * can see. So the render is read back into the console framebuffer instead, and the 2D scanout that already
	 * works puts that on the screen. vrend falls back to the resource's own backing when the transfer names no
	 * iovec (vrend_renderer.c: check_transfer_iovec), which is why attaching it first is what makes this work.
	 */
	{
		struct virtio_gpu_resource_attach_backing fb_attach = { 0 };
		struct virtio_gpu_mem_entry fb_entry = { 0 };
		uint8_t command[sizeof(fb_attach) + sizeof(fb_entry)];

		fb_entry.addr = kvtophys(vinfo.v_baseaddr);
		fb_entry.length = (uint32_t)(vinfo.v_rowbytes * panel_height);
		fb_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
		fb_attach.resource_id = gpu_resource_3d;
		fb_attach.nr_entries = 1;
		memcpy(command, &fb_attach, sizeof(fb_attach));
		memcpy(command + sizeof(fb_attach), &fb_entry, sizeof(fb_entry));
		if (gpu_command(command, sizeof(command)) != VIRTIO_GPU_RESP_OK_NODATA) {
			printf("gpu: 3D: could not attach the framebuffer as the 3D resource's backing\n");
			return;
		}
	}
	{
		struct {
			struct virtio_gpu_ctrl_hdr hdr;
			struct virtio_gpu_box box;
			uint64_t offset;
			uint32_t resource_id, level, stride, layer_stride;
		} xfer = { 0 };

		xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
		xfer.hdr.ctx_id = GPU_CTX_ID;
		xfer.box.w = panel_width;
		xfer.box.h = panel_height;
		xfer.box.d = 1;
		xfer.resource_id = gpu_resource_3d;
		xfer.stride = vinfo.v_rowbytes;
		xfer.layer_stride = vinfo.v_rowbytes * panel_height;
		if (gpu_command(&xfer, sizeof(xfer)) != VIRTIO_GPU_RESP_OK_NODATA) {
			printf("gpu: 3D: TRANSFER_FROM_HOST_3D refused; the render stays in the renderer\n");
			return;
		}
	}
	/* and the console framebuffer, now holding the GPU's picture, onto the scanout that works */
	gpu_refresh();

	/* The whole chain, and it is the device's own renderer that drew the pixels: a virgl2 context, a 3D
	 * resource with the console framebuffer attached as its backing, a CLEAR_TEXTURE the host's virglrenderer
	 * executed as glClearTexSubImage, and a TRANSFER_FROM_HOST_3D that read the result back into that
	 * framebuffer. The 2D scanout above then puts it on the screen -- which is why the screen changes colour
	 * here. Nothing in the guest rasterised; the host's GL did. */
	printf("gpu: 3D: resource %u drawn by the host's GL renderer, read back into the framebuffer, on scanout 0\n",
	    gpu_resource_3d);
}

static bool
queue_make(void)
{
	struct vq_desc *desc = (struct vq_desc *)(ring_mem + OFFSET_DESC);

	reg_write(VIRTIO_QUEUE_SEL, QUEUE_CONTROL);
	if (reg_read(VIRTIO_QUEUE_NUM_MAX) < 2) {
		printf("gpu: the control queue holds only %u\n", reg_read(VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	/* descriptor 0 carries the command, descriptor 1 the reply, and they are always the same two */
	desc[0].addr = kvtophys(buffer_mem);
	desc[1].addr = kvtophys(buffer_mem + IO_PAGE);
	reg_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(VIRTIO_QUEUE_DESC, kvtophys(ring_mem) + OFFSET_DESC);
	reg_write64(VIRTIO_QUEUE_DRIVER, kvtophys(ring_mem) + OFFSET_AVAIL);
	reg_write64(VIRTIO_QUEUE_DEVICE, kvtophys(ring_mem) + OFFSET_USED);
	__builtin_arm_dmb(0xb);
	reg_write(VIRTIO_QUEUE_READY, 1);
	return true;
}

/* ---- the coarse comparison ---------------------------------------------------------------------------------
 * Rows are compared GPU_BAND_ROWS at a time, as one block, and the answer is which bands differ -- not a pixel
 * rectangle. That is the coarseness the shadow is declared with above, and it is deliberate: a band with one changed
 * byte marks every row in it, and a gap between two dirty bands is transferred with them because one command carries
 * a contiguous span and two commands cannot. What it buys is one memcmp per 64 rows instead of 64, which is what
 * matters when the bytes have to come off device memory anyway.
 */
#define GPU_BAND_ROWS           64              /* rows compared at once; see above for what the coarseness costs */

/* The first and one-past-the-last row of a band. The last band is short when the height is not a multiple of the
 * band's, which is why the end is clipped rather than computed as row + GPU_BAND_ROWS. */
static uint32_t
gpu_band_top(uint32_t band)
{
	return band * GPU_BAND_ROWS;
}

static uint32_t
gpu_band_end(uint32_t band)
{
	uint32_t end = (band + 1) * GPU_BAND_ROWS;

	return end > panel_height ? panel_height : end;
}

static uint32_t
gpu_band_count(void)
{
	return (panel_height + GPU_BAND_ROWS - 1) / GPU_BAND_ROWS;
}

/* Does this band differ from what the device was last given? An empty band (only possible when the panel is shorter
 * than one band, which panel_height == 0 would make, and gpu_start() has already refused that) matches, and says so
 * rather than being taken as a memcmp of zero bytes, which is what it would otherwise be. */
static bool
gpu_band_differs(uint32_t band)
{
	const uint8_t *now = (const uint8_t *)(uintptr_t)vinfo.v_baseaddr;
	uint32_t top = gpu_band_top(band);
	uint32_t rows = gpu_band_end(band) - top;
	size_t at = (size_t)top * vinfo.v_rowbytes;

	return rows != 0 && memcmp(now + at, shadow_mem + at, (size_t)rows * vinfo.v_rowbytes) != 0;
}

/*
 * The span of rows that differ from the last upload, COARSELY.
 *
 * Returns false when there is nothing to compare against and the only honest answer is the whole panel: no shadow
 * (never allocated, or the allocation failed), no successful upload in it yet, or a framebuffer whose pitch is not
 * the resource's stride. That last one is not a corner case to brush off: the device walks the guest's memory with
 * ITS OWN stride (QEMU's virtio_gpu_transfer_to_host_2d, hw/display/virtio-gpu.c:500 -- s = pixman_image_get_stride,
 * set from the resource's width at :243), so when the pitch and the stride differ, row N of the framebuffer is not
 * row N of the picture, and a comparison of the two would mark dirty rows that did not change and miss rows that
 * did. With them equal -- which they are for the panel's own geometry, 1125 * 4 -- the comparison is row for row.
 *
 * When it returns true, *top and *rows are the span of GPU_BAND_ROWS-row bands that differ, and *rows is 0 if none
 * did (the device already has this picture).
 *
 * WHAT IT COSTS. A band that differs ends its memcmp at the first differing byte; a band that matches is compared in
 * full. So a frame with a change near the top is cheap, and the quiet case -- nothing changed, which is most of the
 * 1 Hz ticks -- is a full compare of the panel, 10.9 MB. At memcmp speed that is a handful of milliseconds, once a
 * second, against a 1 Hz transfer that moves the same bytes through the ring anyway; it is not free, and it is the
 * price of not asking the device for its own copy. There is one band comparison per band, 38 of them for this
 * panel's height, so the call count is bounded by the geometry rather than by anything a caller does.
 *
 * The scan goes forward from the top to the first differing band, then backward from the bottom to the last one,
 * because those two are all the span needs. The gap between them travels with them: one command carries a contiguous
 * span and two commands cannot (each is a round trip through the ring), and an e-ink partial update is one rect
 * anyway.
 *
 * It reads the framebuffer while the console may still be writing it, and that is allowed to give either answer. A
 * band written between the comparison and the transfer is transferred by the transfer itself, because the transfer
 * reads the same memory a moment later. A band written after its own comparison and before this function's caller
 * gets to transfer it is NOT in this span -- and it is not lost either: the shadow still holds the old picture, so
 * the next tick, one second later, compares it, finds it different, and sends it. The failure mode is a band that
 * appears on the panel up to one refresh late, never a band that never appears.
 */
static bool
gpu_damage_span(uint32_t *top, uint32_t *rows)
{
	uint32_t bands = gpu_band_count();
	uint32_t first, last;

	*top = 0;
	*rows = 0;
	if (shadow_mem == NULL || !shadow_valid) {
		return false;
	}
	/* the pitch has to be the device's stride, or row N here is not row N there: see above */
	if ((uint64_t)vinfo.v_rowbytes != (uint64_t)panel_width * 4) {
		return false;
	}
	for (first = 0; first < bands; first++) {
		if (gpu_band_differs(first)) {
			break;
		}
	}
	if (first == bands) {
		return true;                    /* nothing differs; rows stays 0 */
	}
	for (last = bands; last > first; last--) {
		if (gpu_band_differs(last - 1)) {
			break;
		}
	}
	*top = gpu_band_top(first);
	*rows = gpu_band_end(last - 1) - *top;
	return true;
}

/* Add one transferred rect to what /dev/hakios-gpu will publish. Ends are exclusive, which is where a union is
 * cheap and stays the empty rect (an end before its start) until something is added. Called with gpu_lock held. */
static void
gpu_damage_add(uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
	uint32_t right = left + width;
	uint32_t bottom = top + height;

	if (width == 0 || height == 0) {
		return;
	}
	if (damage_right <= damage_left || damage_bottom <= damage_top) {
		damage_left = left;
		damage_top = top;
		damage_right = right;
		damage_bottom = bottom;
	} else {
		if (left < damage_left) {
			damage_left = left;
		}
		if (top < damage_top) {
			damage_top = top;
		}
		if (right > damage_right) {
			damage_right = right;
		}
		if (bottom > damage_bottom) {
			damage_bottom = bottom;
		}
	}
	damage_transfers++;
}

/* The picture, from the console's framebuffer to the device, then on to the scanout.
 *
 * gpu_lock is taken once, at the end, and never across a command: see the lock's own comment for why it stays that
 * small.
 */
static bool
gpu_refresh(void)
{
	struct virtio_gpu_transfer_to_host_2d transfer = { 0 };
	struct virtio_gpu_resource_flush flush = { 0 };
	uint32_t top = 0, rows = panel_height;
	uint32_t type;

	if (!attached || vinfo.v_baseaddr == 0) {
		return false;
	}
	/*
	 * Two uploads: the whole panel when there is nothing to compare with, and otherwise only the span that
	 * changed. The whole panel is what this driver has always sent, and it is still what the first upload sends.
	 *
	 * A comparison that says nothing changed returns here without a command, and that is a successful refresh:
	 * the device has this picture already, and sending it again would be the e-ink failure the rect exists to
	 * avoid (a full-panel waveform for a frame that did not change).
	 */
	if (!gpu_damage_span(&top, &rows)) {
		top = 0;
		rows = panel_height;
	}
	if (rows == 0) {
		return true;
	}
	transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	transfer.r.y = top;
	transfer.r.width = panel_width;
	transfer.r.height = rows;
	/*
	 * The offset is into the RESOURCE, not into the framebuffer: the device reads row `top` of the picture by
	 * adding its own stride per row to this, and the stride is the resource's width * 4 (virtio-gpu.c:243). So
	 * the source row is top * stride, which is only vinfo.v_rowbytes * top when the two agree -- the case
	 * gpu_damage_span() insists on before it lets a partial transfer happen at all. A full-panel upload has
	 * top 0 here, so this expression is 0 and the change does not touch it; the padded-pitch problem of a
	 * full-panel upload (the device reads contiguous bytes at its own stride, so rows run short) is old, and
	 * fixing it is a different change than this one.
	 */
	transfer.offset = (uint64_t)top * ((uint64_t)panel_width * 4);
	transfer.resource_id = resource_id;
	type = gpu_command(&transfer, sizeof(transfer));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		failures++;
		return false;
	}
	flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	flush.r.y = top;
	flush.r.width = panel_width;
	flush.r.height = rows;
	flush.resource_id = resource_id;
	type = gpu_command(&flush, sizeof(flush));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		failures++;
		return false;
	}
	refreshes++;
	/*
	 * The shadow moves only after both commands answered: a transfer that failed left the device with the old
	 * picture, and a shadow that claimed otherwise would keep the next upload from ever sending it.
	 *
	 * The rect is added whether or not there is a shadow, and it is the rect that was actually just transferred.
	 * Without a shadow that is the whole panel, every time -- which is the truth, and what a caller would be told.
	 * gpu_damage_add() is a union, so a caller slower than the refresh sees the union of every transfer since its
	 * last read rather than only the last one.
	 */
	if (shadow_mem != NULL) {
		uint8_t *base = (uint8_t *)(uintptr_t)vinfo.v_baseaddr;
		size_t at = (size_t)top * vinfo.v_rowbytes;

		memcpy(shadow_mem + at, base + at, (size_t)rows * vinfo.v_rowbytes);
		shadow_valid = true;
	}
	lck_mtx_lock(&gpu_lock);
	gpu_damage_add(0, top, panel_width, rows);
	lck_mtx_unlock(&gpu_lock);
	return true;
}

static void
gpu_tick(thread_call_param_t arg __unused, thread_call_param_t unused __unused)
{
	uint64_t deadline;

	gpu_refresh();
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);      /* the panel is not 60 Hz, and this is not it */
	thread_call_enter_delayed(gpu_call, deadline);
}

/* ---- /dev/hakios-gpu: the damage rect, asked for by whoever drives the panel ----------------------------------
 * The device exists only when virtio_gpu=1 has a working 2D scanout: with no device there is no transfer, so there
 * is no rect, and a node that always answered "nothing changed" would be a lie a caller could not tell from the
 * truth. It is made where the other boot-time character devices are made -- in bsd_init.c's DEVFS block, right after
 * devfs_kernel_mount (:990, and the hakios_* calls from :992) -- for the reason hakios_epd_init()'s own comment
 * gives: a node made before that mount is not in /dev.
 *
 * Only this command, and no write() or read(): the rect is not a stream and there is nothing a caller may send.
 * Everything else is ENOTTY.
 */
static int
gpu_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
	/*
	 * The !attached check is belt and braces, and it is worth saying that plainly: gpu_node_make() runs only after
	 * gpu_start() has succeeded, so the node does not exist in any state where attached is false, and attached
	 * never goes back to false once it is true. It stays because a descriptor can outlive the open that got it,
	 * and answering ENXIO is cheaper to reason about than trusting that no such path is ever added.
	 *
	 * An empty rect here is the honest answer, and it is distinguishable from "no picture is being transferred"
	 * because the latter is ENXIO and the former is success with width == height == 0.
	 */
	switch (cmd) {
	case (u_long)HAKIOS_VIRTIO_GPU_DAMAGE:
		{
			struct hakios_gpu_damage *d = (struct hakios_gpu_damage *)data;

			if (!attached) {
				return ENXIO;
			}
			lck_mtx_lock(&gpu_lock);
			/* the empty rect is the "ends not ordered" form the accumulator starts in */
			if (damage_right <= damage_left || damage_bottom <= damage_top) {
				d->top = 0;
				d->left = 0;
				d->width = 0;
				d->height = 0;
			} else {
				d->left = damage_left;
				d->top = damage_top;
				d->width = damage_right - damage_left;
				d->height = damage_bottom - damage_top;
			}
			d->transfers = damage_transfers;
			damage_left = damage_top = damage_right = damage_bottom = 0;
			damage_transfers = 0;
			lck_mtx_unlock(&gpu_lock);
			return 0;
		}
	default:
		return ENOTTY;
	}
}

/* An open is allowed as soon as there is a node, and a node is made only once the scanout is up, so this is 0 in
 * practice -- see gpu_ioctl()'s own comment for why the check stays. eno_opcl is not usable here: it is enodev,
 * which answers ENODEV and would refuse the open of a node that exists. */
static int
gpu_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return attached ? 0 : ENXIO;
}

static int
gpu_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw gpu_cdevsw = {
	.d_open = gpu_open, .d_close = gpu_close, .d_read = eno_rdwrt, .d_write = eno_rdwrt,
	.d_ioctl = gpu_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* The node, and not a failure worth stopping the scanout for: a device without this door still puts the picture on
 * the screen, it just leaves the panel side with nothing to be told. Says so, and carries on.
 *
 * devfs_make_node returns void *: an unset node or a refused fixup comes back as NULL, which is what the other
 * character devices in this tree test for (hakios.epd.c, hakios.fb.c, hakios.input.c). */
static void
gpu_node_make(void)
{
	int major = cdevsw_add(-1, &gpu_cdevsw);
	void *node;

	if (major < 0) {
		printf("gpu: cannot add the cdevsw for /dev/hakios-gpu; the damage rect has no door\n");
		return;
	}
	node = devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-gpu");
	if (node == NULL) {
		printf("gpu: cannot create /dev/hakios-gpu; the damage rect has no door, the scanout is unaffected\n");
	}
}

static bool
gpu_start(void)
{
	vm_offset_t bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	struct virtio_gpu_resource_create_2d create = { 0 };
	struct virtio_gpu_resource_attach_backing attach = { 0 };
	struct virtio_gpu_mem_entry entry;
	struct virtio_gpu_set_scanout scanout = { 0 };
	struct virtio_gpu_resp_display_info info;
	uint32_t features, type;
	int slot;

	for (slot = 0; slot < VIRTIO_SLOTS; slot++) {
		virtio = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(VIRTIO_MAGIC) == 0x74726976 && reg_read(VIRTIO_DEVICE_ID) == VIRTIO_ID_GPU) {
			break;
		}
	}
	if (slot == VIRTIO_SLOTS) {
		printf("gpu: no virtio GPU (start QEMU with -device virtio-gpu-device)\n");
		return false;
	}
	if (reg_read(VIRTIO_VERSION) != 2) {
		printf("gpu: slot %d is a legacy transport; start QEMU with -global virtio-mmio.force-legacy=false\n", slot);
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0) {
		printf("gpu: no boot framebuffer to put on the device\n");
		return false;
	}
	panel_width = vinfo.v_width;
	panel_height = vinfo.v_height;

	reg_write(VIRTIO_STATUS, 0);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(VIRTIO_DEVICE_FEATURES_SEL, 0);
	features = reg_read(VIRTIO_DEVICE_FEATURES);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
	/* VIRTIO_GPU_F_VIRGL only when the device offers it: asking for a feature it does not have gets FEATURES_OK
	 * refused, and the 2D scanout would then never come up either */
	reg_write(VIRTIO_DRIVER_FEATURES, (features & VIRTIO_GPU_F_VIRGL) ? VIRTIO_GPU_F_VIRGL : 0);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(VIRTIO_DRIVER_FEATURES, 1);                   /* VIRTIO_F_VERSION_1 */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("gpu: slot %d: features refused (the device offers %#x)\n", slot, features);
		return false;
	}
	if (kmem_alloc(kernel_map, &ring_mem, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &buffer_mem, 2 * IO_PAGE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	if (!queue_make()) {
		return false;
	}
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

	/* what the display says it is, before asking for a scanout of that size */
	info.hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
	if (gpu_command(&info, sizeof(info.hdr)) == VIRTIO_GPU_RESP_OK_DISPLAY_INFO && info.pmodes[0].enabled) {
		printf("gpu: display 0 says %ux%u; the panel is %ux%u\n", info.pmodes[0].r.width, info.pmodes[0].r.height,
		    panel_width, panel_height);
	}

	resource_id = 1;
	create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
	create.resource_id = resource_id;
	create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
	create.width = panel_width;
	create.height = panel_height;
	type = gpu_command(&create, sizeof(create));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: RESOURCE_CREATE_2D refused (%#x)\n", type);
		return false;
	}
	/* the console's framebuffer as the resource's backing: the device reads the same memory XNU draws into */
	entry.addr = kvtophys(vinfo.v_baseaddr);
	entry.length = (uint32_t)(vinfo.v_rowbytes * panel_height);
	entry.padding = 0;
	attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	attach.resource_id = resource_id;
	attach.nr_entries = 1;
	{
		/* header then the one entry, in the same command buffer: the device wants them contiguous */
		uint8_t command[sizeof(attach) + sizeof(entry)];

		memcpy(command, &attach, sizeof(attach));
		memcpy(command + sizeof(attach), &entry, sizeof(entry));
		type = gpu_command(command, sizeof(command));
	}
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: RESOURCE_ATTACH_BACKING refused (%#x)\n", type);
		return false;
	}
	scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
	scanout.r.width = panel_width;
	scanout.r.height = panel_height;
	scanout.scanout_id = 0;
	scanout.resource_id = resource_id;
	type = gpu_command(&scanout, sizeof(scanout));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		/* SET_SCANOUT is the one command whose failure leaves the queue looking dead; ask something that answered
		 * before it, so the line says which of the two this is: the command refused, or the ring stopped */
		uint32_t probe;

		info.hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
		probe = gpu_command(&info, sizeof(info.hdr));
		printf("gpu: SET_SCANOUT refused (%#x); a GET_DISPLAY_INFO after it answered %#x%s\n", type, probe,
		    probe == 0 ? " -- the queue stopped there, it is not the command" : "");
		return false;
	}
	attached = true;
	printf("gpu: virtio GPU in slot %d: a %ux%u 2D resource on scanout 0, driven from the console's framebuffer\n",
	    slot, panel_width, panel_height);
	/* the 3D half, after the 2D one is already showing: a refusal here costs the context, not the screen */
	if (features & VIRTIO_GPU_F_VIRGL) {
		if (gpu_ctx_create()) {
			gpu_render_3d();
		}
	} else {
		printf("gpu: this device has no virgl (no VIRTIO_GPU_F_VIRGL), so there is no 3D half to use\n");
	}
	return true;
}

/*
 * The shadow the comparison reads, allocated once the scanout is up. Failure is not fatal: gpu_damage_span() answers
 * "cannot tell" and every upload is the whole panel, which is exactly what this driver did before there was a
 * shadow. round_page because kmem_alloc hands out pages, not bytes.
 */
static void
gpu_shadow_alloc(void)
{
	/*
	 * The product is formed in uint64_t: pitch times height is 10.9 MB for this panel, which fits, but a padded
	 * pitch on a taller framebuffer does not, and a 32-bit multiply that wrapped would ask for a small allocation
	 * and then have gpu_damage_span() compare against the end of it.
	 */
	uint64_t want = (uint64_t)vinfo.v_rowbytes * panel_height;
	vm_size_t size = (vm_size_t)round_page((vm_size_t)want);
	vm_offset_t addr;

	/* round_page rounding UP is the normal case; rounding DOWN means the vm_size_t it was asked for had already
	 * wrapped, and the allocation would be smaller than the framebuffer it is a copy of. */
	if (want == 0 || (uint64_t)size < want) {
		printf("gpu: the framebuffer is too large to shadow; every upload will be the whole panel\n");
		return;
	}
	/* kmem_alloc answers a vm_offset_t, which is what ring_mem and buffer_mem above are too; the comparison wants
	 * a byte pointer over it, and that cast is made once here rather than at every use. */
	if (kmem_alloc(kernel_map, &addr, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("gpu: no memory for the damage shadow; every upload will be the whole panel\n");
		shadow_mem = NULL;
		return;
	}
	shadow_mem = (uint8_t *)addr;
}

void
hakios_virtio_gpu_init(void)
{
	uint32_t enabled = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("virtio_gpu", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	(void)PE_parse_boot_argn("gpu_trace", &gpu_trace, sizeof(gpu_trace));
	if (!gpu_start()) {
		return;
	}
	/* before the first upload, so the first one still goes whole (the shadow has no picture in it yet) and every
	 * one after it can be the difference */
	gpu_shadow_alloc();
	gpu_node_make();                        /* /dev/hakios-gpu: the rect this driver will transfer, for the panel */
	gpu_refresh();                          /* the first picture, before anything else asks */
	gpu_call = thread_call_allocate(gpu_tick, NULL);
	if (gpu_call != NULL) {
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(gpu_call, deadline);
	}
}
