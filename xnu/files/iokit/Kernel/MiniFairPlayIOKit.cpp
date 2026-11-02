/*
 * MiniFairPlayIOKit: what fairplayd asks the kernel for, on a machine without a Secure Enclave.
 *
 * WHAT THIS IS. fairplayd (System/Library/PrivateFrameworks/FairPlaySVCS.framework's agent) opens
 * com_apple_driver_FairPlayIOKitUserClient and asks it to set up a session and to unwrap keys. On a real
 * device the answer comes from the SEP: a key that never leaves it, and an attestation that says which
 * hardware it is. Nothing here can do either. This driver exists so the client opens, the selectors are
 * answered, and the code that calls them runs far enough to say what it wanted -- which is the honest
 * shape of the port today.
 *
 * WHAT IT DOES NOT DO, and must not be read as doing:
 *
 *   - No attestation. The "certificate" it hands back is a fixed, self-made buffer: it is not signed by
 *     Apple, it does not chain to Apple's root, and no server will accept it.
 *   - No secret. Keys "unwrapped" here are mixed with a fixed pattern, exactly like MiniAppleKeyStore's
 *     wrapping: reversible, and worth nothing.
 *   - No DRM. FairPlay-protected media will not decrypt.
 *
 * So a caller that only needs the client to exist and to answer gets an answer; a caller that needs a
 * real SEP fails, at the far end, on a signature that is not there. That failure is the point: inventing
 * a signature that was accepted would be the one thing this port must not do.
 *
 * THE SELECTORS, read off the user-space side of Darwin userland 16.5 (the numbers are the ones the
 * FairPlaySVCS agent passes as externalMethod's selector, not an Apple header -- none is public):
 *
 *   0x00  open/check                       -> success
 *   0x01  create a session (in: a blob)    -> out: a session id
 *   0x02  close a session                  -> success
 *   0x03  the device's "certificate"       -> out: our fixed buffer, NOT an attestation
 *   0x04  HW info (out: a small structure)  -> out: flags that say there is no SEP
 *
 * Anything else is refused and logged once, as MiniAppleKeyStore does, so a start-up that walks the
 * space shows up in the console log rather than failing silently.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

enum {
	kFPOpen                 = 0x00,         /* in: nothing; out: nothing */
	kFPCreateSession        = 0x01,         /* in: a blob;   out: a session id */
	kFPCloseSession         = 0x02,
	kFPGetCertificate       = 0x03,         /* out: bytes, which are NOT a real certificate */
	kFPHardwareInfo         = 0x04,         /* out: flags */
	kFPUnwrapKey            = 0x05,         /* in: a wrapped key; out: the "key" */
};

/* bit 0: there is a SEP answering. This machine says 0, because there is not one. */
#define kFPHardwareNoSEP        0x1

/* a session id that changes per call, so a caller that compares two does not read them as equal */
static uint32_t gFPSessionId = 0x46500001;
static uint64_t gFPLogged[2];           /* selectors already reported */
static uint8_t  gFPKeys;                /* how many "keys" have been handed out */

/* Not a certificate. A stand-in of the right shape, with a mark that says so in its first bytes. */
static const uint8_t kFPCertificateBytes[] = {
	0x30, 0x2a,                              /* SEQUENCE, 42 bytes */
	0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05,        /* OID 1.2.840.113549.1.1.5 */
	0x03, 0x1d, 0x00,                        /* BIT STRING */
	'H', 'a', 'k', 'i', 'O', 'S',            /* the mark: this is ours, not Apple's */
	'n', 'o', 'S', 'E', 'P',
};

static void
fpUnwrapMix(uint8_t * bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x9e + 29 * i);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class FairPlayIOKit : public IOService
{
	OSDeclareDefaultStructors(FairPlayIOKit);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(FairPlayIOKit, IOService);

bool
FairPlayIOKit::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "FairPlayIOKitUserClient");
	setName("FairPlayIOKit");
	registerService();
	return true;
}

class FairPlayIOKitUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(FairPlayIOKitUserClient);

	static IOReturn answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(FairPlayIOKitUserClient, IOUserClient);

IOReturn
FairPlayIOKitUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* A structure answer; a buffer too big for the call arrives as a memory descriptor, as with the keybag. */
IOReturn
FairPlayIOKitUserClient::answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length)
{
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret;

		if (out->getLength() < length) {
			return kIOReturnNoSpace;
		}
		ret = out->prepare();
		if (ret == kIOReturnSuccess) {
			out->writeBytes(0, bytes, length);
			out->complete();
			args->structureOutputDescriptorSize = (uint32_t) length;
		}
		return ret;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < length) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, bytes, length);
	args->structureOutputSize = (uint32_t) length;
	return kIOReturnSuccess;
}

IOReturn
FairPlayIOKitUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	uint64_t * out = args->scalarOutput;

	switch (selector) {
	case kFPOpen:
		return kIOReturnSuccess;

	case kFPCreateSession:
		gFPSessionId++;
		if (args->scalarOutputCount >= 1) {
			out[0] = gFPSessionId;
			args->scalarOutputCount = 1;
		}
		return kIOReturnSuccess;

	case kFPCloseSession:
		return kIOReturnSuccess;

	case kFPGetCertificate:
		/* Handed out, but say once, plainly, what it is: a caller reading only the return code would
		 * otherwise take this for an attestation, and the console is where the difference is visible. */
		if ((gFPLogged[0] & 1) == 0) {
			gFPLogged[0] |= 1;
			IOLog("MiniFairPlayIOKit: certificate requested; handing out a placeholder that is NOT signed by "
			    "Apple and will not be accepted by any server (there is no Secure Enclave here)\n");
		}
		return answer(args, kFPCertificateBytes, sizeof(kFPCertificateBytes));

	case kFPHardwareInfo: {
		uint8_t flags = kFPHardwareNoSEP;

		return answer(args, &flags, sizeof(flags));
	}

	case kFPUnwrapKey: {
		uint8_t key[64];
		size_t length = args->structureInputSize;

		if (args->structureInput == NULL || length == 0 || length > sizeof(key)) {
			return kIOReturnBadArgument;
		}
		memcpy(key, args->structureInput, length);
		fpUnwrapMix(key, length);
		gFPKeys++;
		return answer(args, key, length);
	}

	default:
		if ((gFPLogged[selector / 64] & (1ULL << (selector % 64))) == 0) {
			char procname[32] = "?";

			gFPLogged[selector / 64] |= 1ULL << (selector % 64);
			proc_selfname(procname, sizeof(procname));
			IOLog("MiniFairPlayIOKit: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; structure %u "
			    "in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
			    args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
		}
		return kIOReturnUnsupported;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniFairPlayIOKitStart(void)
{
	/* user space opens it by class name, and by the path IOService:/IOResources/FairPlayIOKit */
	IOService * resources = IOService::getResourceService();
	FairPlayIOKit * fp = OSTypeAlloc(FairPlayIOKit);

	if (resources == NULL || fp == NULL || !fp->init() || !fp->attach(resources)) {
		OSSafeReleaseNULL(fp);
		IOLog("MiniFairPlayIOKit: cannot create the service\n");
		return;
	}
	if (!fp->start(resources)) {
		fp->detach(resources);
		fp->release();
		return;
	}
	/* a line at boot, as the other stand-ins have: without one there is no way to tell "registered" from
	 * "never ran", and fairplayd's failure would read the same either way */
	IOLog("MiniFairPlayIOKit: /IOResources/FairPlayIOKit, class FairPlayIOKitUserClient (no SEP: no attestation)\n");
	fp->release();
}
