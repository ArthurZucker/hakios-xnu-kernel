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
 */
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer: what the text console draws into */

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
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0202
#define VIRTIO_GPU_CMD_SUBMIT_3D           0x0204
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
#define VIRTIO_GPU_CAPSET_VIRGL2        2       /* what a virgl2 guest asks for, and what QEMU offers first */

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
	ctx.hdr.ctx_id = 0;                     /* the device's context 0: the one every 3D command names */
	ctx.nlen = sizeof(name) - 1;
	ctx.context_init = VIRTIO_GPU_CAPSET_VIRGL2;
	memcpy(ctx.debug_name, name, sizeof(name) - 1);
	type = gpu_command(&ctx, sizeof(ctx));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		printf("gpu: CTX_CREATE (virgl2) refused (%#x); the 3D half of this device stays unused\n", type);
		return false;
	}
	printf("gpu: 3D: a virgl2 context on ctx 0 -- the device made a GL context for it on the host\n");
	return true;
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

/* The picture, from the console's framebuffer to the device, then on to the scanout. */
static bool
gpu_refresh(void)
{
	struct virtio_gpu_transfer_to_host_2d transfer = { 0 };
	struct virtio_gpu_resource_flush flush = { 0 };
	uint32_t type;

	if (!attached || vinfo.v_baseaddr == 0) {
		return false;
	}
	transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	transfer.r.width = panel_width;
	transfer.r.height = panel_height;
	transfer.resource_id = resource_id;
	type = gpu_command(&transfer, sizeof(transfer));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		failures++;
		return false;
	}
	flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	flush.r.width = panel_width;
	flush.r.height = panel_height;
	flush.resource_id = resource_id;
	type = gpu_command(&flush, sizeof(flush));
	if (type != VIRTIO_GPU_RESP_OK_NODATA) {
		failures++;
		return false;
	}
	refreshes++;
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
		gpu_ctx_create();
	} else {
		printf("gpu: this device has no virgl (no VIRTIO_GPU_F_VIRGL), so there is no 3D half to use\n");
	}
	return true;
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
	gpu_refresh();                          /* the first picture, before anything else asks */
	gpu_call = thread_call_allocate(gpu_tick, NULL);
	if (gpu_call != NULL) {
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(gpu_call, deadline);
	}
}
