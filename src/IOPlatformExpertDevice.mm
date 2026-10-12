#include "IOPlatformExpertDevice.h"
#import <Foundation/NSString.h>
#import <Foundation/NSData.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSCharacterSet.h>
#include <cstring>

// Platform expert strings are NUL-terminated data
static NSData* dataForString(NSString* string)
{
	const char* utf8 = [string UTF8String];
	return [NSData dataWithBytes: utf8 length: strlen(utf8) + 1];
}

const char* IOPlatformExpertDevice::className() const
{
	return "IOPlatformExpertDevice";
}

NSDictionary* IOPlatformExpertDevice::matchingDictionary()
{
	return @{
		@"IOProviderClass": @"IOPlatformExpertDevice",
		@"IOClass": @"IOPlatformExpertDevice",
	};
}

NSDictionary* IOPlatformExpertDevice::getProperties()
{
	// Fixed MacBookPro16,1 identity; the Linux DMI values describe the host hardware, not the Mac we emulate
	return @{
		@"manufacturer": dataForString(@"Apple Inc."),
		@"model": dataForString(@"MacBookPro16,1"),
		@"product-name": dataForString(@"MacBookPro16,1"),
		@"board-id": dataForString(@"Mac-E1008331FDC96864"),
		@"IOPlatformUUID": @"6F3B2A1E-8C4D-4E5F-9A7B-1D2C3E4F5A6B",
		@"IOPlatformSerialNumber": @"C02ZL0GVMD6N",
	};
}

bool IOPlatformExpertDevice::conformsTo(const char* className)
{
	if (std::strcmp(className, "IOPlatformDevice") == 0)
		return true;
	return IOService::conformsTo(className);
}

void IOPlatformExpertDevice::registerSelf(ServiceRegistry* targetServiceRegistry)
{
	static IOPlatformExpertDevice* device = new IOPlatformExpertDevice;

	targetServiceRegistry->registerService(device);
	device->registerInPlane(kIOServicePlane, "Platform", IORegistryEntry::root());
}
