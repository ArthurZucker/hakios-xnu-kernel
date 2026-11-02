/*
 * Stand-ins for Apple's closed kernel extensions: just enough of each for the stock Darwin
 * user space to work on a machine without Apple hardware. The platform expert
 * (GenericARMPE) starts them once the registry is up.
 *
 *   MiniIOSurface.cpp              IOSurface: pixel buffers shared between processes
 *   MiniIOMobileFramebuffer.cpp    IOMobileFramebuffer: the display (the boot framebuffer)
 *   MiniAppleKeyStore.cpp          AppleKeyStore: the keybag of a device without a passcode
 *   MiniFairPlayIOKit.cpp          FairPlayIOKit: the client fairplayd opens; no SEP, so no attestation
 *   MiniIONetworkStack.cpp         IONetworkStack: what configd's interface namer waits for
 */
#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H

#include <IOKit/IOService.h>

void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniFairPlayIOKitStart(void);
void MiniIONetworkStackStart(void);

#endif /* _IOKIT_MINIDRIVERS_H */
