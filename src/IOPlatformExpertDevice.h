#ifndef IOKITD_IOPLATFORMEXPERTDEVICE_H
#define IOKITD_IOPLATFORMEXPERTDEVICE_H

#include "IOService.h"
#include "ServiceRegistry.h"

// The root platform device (what IOServiceMatching("IOPlatformExpertDevice") finds on macOS).
// Its properties describe the host machine as reported by the Linux kernel's DMI tables.
// Identifiers the host does not expose to us (serial number, UUID) are left out.
class IOPlatformExpertDevice : public IOService
{
public:
	const char* className() const override;
	NSDictionary* matchingDictionary() override;
	NSDictionary* getProperties() override;
	bool conformsTo(const char* className) override;

	static void registerSelf(ServiceRegistry* targetServiceRegistry);
};

#endif
