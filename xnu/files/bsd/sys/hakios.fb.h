/*
 * /dev/hakios-fb: the panel, writable from user space (devices/b651/drivers/display/hakios.fb.c), with boot-arg
 * fbdev=1. What it is for, and what it is not (it is not a GPU): devices/b651/SIM.md.
 */
#ifndef _SYS_HAKIOS_FB_H_
#define _SYS_HAKIOS_FB_H_

#include <sys/types.h>

#ifdef KERNEL
/* Boot-arg fbdev=1: /dev/hakios-fb exists over the boot framebuffer -- the one virtio_gpu=1 scanouts. Called from
 * bsd_init with the other boot-time drivers. */
void hakios_fb_init(void);
#endif /* KERNEL */

#endif /* !_SYS_HAKIOS_FB_H_ */
