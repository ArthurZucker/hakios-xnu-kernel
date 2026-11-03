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

/*
 * "These pixels were just painted, in the framebuffer" -- the text console's way of telling the panel it has work,
 * in pixels from the panel's top left. Called from vc_paint_char() and vc_clear_screen(), which is every write the
 * console makes, and from nowhere else yet.
 *
 * It is a NOTIFICATION and returns nothing, on purpose. The console is the wrong place to decide a waveform, and it
 * must not block: it runs under the console's own lock on the boot path, often before the panel driver has even
 * attached. So this only widens a rectangle the driver polls, and the driver decides when, with which waveform, and
 * whether the panel is there at all. Called before hakios_epd_init() it is remembered, not dropped, so the page the
 * console paints during boot is the first thing the panel is told about once there is a panel to tell.
 */
void hakios_epd_console_dirty(uint32_t top, uint32_t left, uint32_t width, uint32_t height);
#endif /* KERNEL */

#endif /* !_SYS_HAKIOS_EPD_H_ */
