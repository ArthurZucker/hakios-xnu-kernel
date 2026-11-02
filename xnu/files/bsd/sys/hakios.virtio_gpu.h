/*
 * QEMU's virtio GPU as the port's scanout (drivers/gpu/hakios.virtio_gpu.c), with boot-arg virtio_gpu=1.
 *
 * The kernel side of the interface, as sys/hakios.epd.h is the e-ink panel's.
 */
#ifndef _SYS_HAKIOS_VIRTIO_GPU_H_
#define _SYS_HAKIOS_VIRTIO_GPU_H_

#include <sys/types.h>

#ifdef KERNEL
/* Boot-arg virtio_gpu=1: a 2D resource on the device's scanout 0, driven from the console's framebuffer once a
 * second. Called from bsd_init with the other boot-time drivers. 2D only: this QEMU has no virgl. */
void hakios_virtio_gpu_init(void);
#endif /* KERNEL */

#endif /* !_SYS_HAKIOS_VIRTIO_GPU_H_ */
