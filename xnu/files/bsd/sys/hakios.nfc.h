/*
 * /dev/hakios-nfc: the ST54's character device (devices/b651/drivers/nfc/hakios.nfc.c), with boot-arg nfc=1.
 *
 * The kernel side of the interface, as sys/hakios.epd.h is the e-ink panel's.
 */
#ifndef _SYS_HAKIOS_NFC_H_
#define _SYS_HAKIOS_NFC_H_

#include <sys/types.h>

#ifdef KERNEL
/* Boot-arg nfc=1: /dev/hakios-nfc exists. Called from bsd_init with the other boot-time drivers. Without the four
 * pins the phone's dtbo has not been read for, the device is made and every command answers ENOTSUP; with
 * nfc_model=1 a MODELED chip stands in, so the state machine above the pins is exercised in the simulator. */
void hakios_nfc_init(void);
#endif /* KERNEL */

#endif /* !_SYS_HAKIOS_NFC_H_ */
