/*
 * /dev/hakios-epd: the e-ink panel's update ABI, for callers outside the driver (the console, the display shim).
 *
 * The driver itself is devices/b651/drivers/display/hakios.epd.c, built only with CONFIG_HAKIOS_EPD. This header is
 * the kernel side of the interface, the way sys/hakios.input.h is /dev/hakios-touch's.
 */
#ifndef _SYS_HAKIOS_EPD_H_
#define _SYS_HAKIOS_EPD_H_

#include <sys/types.h>

#ifdef KERNEL
/* Boot-arg epd=1: /dev/hakios-epd exists (devices/b651/drivers/display/hakios.epd.c). Called from bsd_init once
 * devfs is mounted, as hakios_input_init() is: a node made before that is not in /dev. */
void hakios_epd_init(void);
#endif /* KERNEL */

#endif /* !_SYS_HAKIOS_EPD_H_ */
