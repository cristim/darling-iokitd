/*
 This file is part of Darling.

 Copyright (C) 2020 Lubos Dolezel

 Darling is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Darling is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PowerAssertions.h"
#include "PowerAssertionsX11.h"
#include "HostPowerInhibit.h"
#include <stdexcept>
#include <iostream>
#include <string>
#include <climits>
#include <CoreFoundation/CoreFoundation.h>
#include <bsm/libbsm.h>
#import <Foundation/NSString.h>

extern "C" {
#include "powermanagementServer.h"
}

class PowerAssertionPreventIdleSleep : public PowerAssertion
{
	struct iokitd_power_inhibit* m_host = nullptr;
	IOReturn activate() override
	{
		const auto* api = iokitd_host_power_api();
		return api && iokitd_power_inhibit_create(api, 0, &m_host) == 0
			? kIOReturnSuccess : kIOReturnNotReady;
	}
	void deactivate() override
	{
		iokitd_power_inhibit_destroy(m_host);
		m_host = nullptr;
	}
};

std::unordered_map<int, AppAssertions*> AppAssertions::m_instances;
static std::unordered_map<std::string, PowerAssertion*> g_assertionKinds;

void initPM()
{
	// Legacy and modern spellings share one backend and one hold count.
	static PowerAssertionPreventIdleSleep idle;
	static PowerAssertionPreventDisplaySleep display;
	g_assertionKinds = {
		{[(NSString*)kIOPMAssertionTypeNoIdleSleep UTF8String], &idle},
		{[(NSString*)kIOPMAssertPreventUserIdleSystemSleep UTF8String], &idle},
		{[(NSString*)kIOPMAssertionTypeNoDisplaySleep UTF8String], &display},
		{[(NSString*)kIOPMAssertPreventUserIdleDisplaySleep UTF8String], &display},
	};
}

IOReturn PowerAssertion::addRef()
{
	if (m_numHoldingApps == 0)
	{
		IOReturn result = activate();
		if (result != kIOReturnSuccess) return result;
	}
	m_numHoldingApps++;
	return kIOReturnSuccess;
}

void PowerAssertion::delRef()
{
	if (m_numHoldingApps > 0 && --m_numHoldingApps == 0)
		deactivate();
}

AppAssertions::AppAssertions(int pid)
{
	m_processSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
	if (!m_processSource)
		throw std::runtime_error("dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC) failed");
	dispatch_source_set_event_handler(m_processSource, ^{
		m_instances.erase(pid);
		delete this;
	});
	dispatch_resume(m_processSource);
}

AppAssertions::~AppAssertions()
{
	for (auto& item : m_heldAssertions)
		if (item.second.enabled) item.second.assertion->delRef();
	dispatch_source_cancel(m_processSource);
	dispatch_release(m_processSource);
}

AppAssertions* AppAssertions::get(int pid, bool create)
{
	auto it = m_instances.find(pid);
	if (it != m_instances.end()) return it->second;
	if (!create) return nullptr;
	try
	{
		AppAssertions* app = new AppAssertions(pid);
		m_instances.emplace(pid, app);
		return app;
	}
	catch (const std::exception& e)
	{
		std::cerr << "AppAssertions::get() failed for PID " << pid << ": " << e.what() << std::endl;
		return nullptr;
	}
}

AppAssertions::HeldAssertion* AppAssertions::getAssertion(int assertionId)
{
	auto it = m_heldAssertions.find(assertionId);
	return it == m_heldAssertions.end() ? nullptr : &it->second;
}

IOReturn AppAssertions::createAssertion(PowerAssertion* which, bool enabled, int* assertionId)
{
	if (m_nextAssertionNumber == INT_MAX) return kIOReturnNoResources;
	if (enabled)
	{
		IOReturn result = which->addRef();
		if (result != kIOReturnSuccess) return result;
	}
	int number = m_nextAssertionNumber++;
	try { m_heldAssertions.emplace(number, HeldAssertion{1, which, enabled}); }
	catch (...)
	{
		if (enabled) which->delRef();
		return kIOReturnNoMemory;
	}
	*assertionId = number;
	return kIOReturnSuccess;
}

IOReturn AppAssertions::setEnabled(HeldAssertion* held, bool enabled)
{
	if (held->enabled == enabled) return kIOReturnSuccess;
	if (enabled)
	{
		IOReturn result = held->assertion->addRef();
		if (result != kIOReturnSuccess) return result;
	}
	else held->assertion->delRef();
	held->enabled = enabled;
	return kIOReturnSuccess;
}

void AppAssertions::killAssertion(int assertionId)
{
	auto it = m_heldAssertions.find(assertionId);
	if (it == m_heldAssertions.end()) return;
	if (it->second.enabled) it->second.assertion->delRef();
	m_heldAssertions.erase(it);
}

// These MIG routines return their IOReturn in return_code and KERN_SUCCESS for
// transport. Consequently they own (and consume) the request's OOL data even
// when validation fails; dispatch_mig_server must not destroy it a second time.
static CFDictionaryRef copyProperties(vm_offset_t props, mach_msg_type_number_t count)
{
	CFDictionaryRef result = nullptr;
	if (props && count)
	{
		CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, (const UInt8*)props, count, kCFAllocatorNull);
		if (data)
		{
			result = (CFDictionaryRef)CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
			CFRelease(data);
		}
	}
	if (props) vm_deallocate(mach_task_self(), props, count);
	if (result && CFGetTypeID(result) != CFDictionaryGetTypeID())
	{
		CFRelease(result);
		result = nullptr;
	}
	return result;
}

static bool assertionLevel(CFDictionaryRef properties, bool* enabled)
{
	CFTypeRef number = CFDictionaryGetValue(properties, kIOPMAssertionLevelKey);
	if (!number) return true;
	int level;
	if (CFGetTypeID(number) != CFNumberGetTypeID() ||
		!CFNumberGetValue((CFNumberRef)number, kCFNumberIntType, &level) ||
		(level != kIOPMAssertionLevelOff && level != kIOPMAssertionLevelOn)) return false;
	*enabled = level == kIOPMAssertionLevelOn;
	return true;
}

kern_return_t _io_pm_assertion_create(mach_port_t server, audit_token_t token,
	vm_offset_t props, mach_msg_type_number_t propsCnt, int* assertion_id,
	int* disableAppSleep, int* enTrIntensity, int* return_code)
{
	*assertion_id = kIOPMNullAssertionID;
	*disableAppSleep = 0;
	*enTrIntensity = -1;
	*return_code = kIOReturnBadArgument;
	CFDictionaryRef properties = copyProperties(props, propsCnt);
	if (!properties) return KERN_SUCCESS;
	CFTypeRef type = CFDictionaryGetValue(properties, kIOPMAssertionTypeKey);
	bool enabled = true;
	CFTypeRef timeout = CFDictionaryGetValue(properties, kIOPMAssertionTimeoutKey);
	double seconds = 0;
	if (timeout && (CFGetTypeID(timeout) != CFNumberGetTypeID() ||
		!CFNumberGetValue((CFNumberRef)timeout, kCFNumberDoubleType, &seconds)))
		*return_code = kIOReturnBadArgument;
	else if (seconds != 0)
		// Timed assertions need expiration scheduling. Do not silently turn
		// a timed request into an indefinite host inhibition.
		*return_code = kIOReturnUnsupported;
	else if (type && CFGetTypeID(type) == CFStringGetTypeID() && assertionLevel(properties, &enabled))
	{
		auto it = g_assertionKinds.find([(NSString*)type UTF8String]);
		if (it == g_assertionKinds.end()) *return_code = kIOReturnUnsupported;
		else
		{
			pid_t callerPid;
			audit_token_to_au32(token, nullptr, nullptr, nullptr, nullptr, nullptr, &callerPid, nullptr, nullptr);
			AppAssertions* app = AppAssertions::get(callerPid);
			*return_code = app ? app->createAssertion(it->second, enabled, assertion_id) : kIOReturnNoResources;
		}
	}
	CFRelease(properties);
	return KERN_SUCCESS;
}

kern_return_t _io_pm_assertion_retain_release(mach_port_t server, audit_token_t token,
	int assertion_id, int action, int* retainCnt, int* disableAppSleep,
	int* enableAppSleep, int* return_code)
{
	*retainCnt = *disableAppSleep = *enableAppSleep = 0;
	*return_code = kIOReturnNotFound;
	pid_t callerPid;
	audit_token_to_au32(token, nullptr, nullptr, nullptr, nullptr, nullptr, &callerPid, nullptr, nullptr);
	AppAssertions* app = AppAssertions::get(callerPid, false);
	auto* held = app ? app->getAssertion(assertion_id) : nullptr;
	if (!held) return KERN_SUCCESS;
	if (action == kIOPMAssertionMIGDoRetain)
	{
		if (held->refcount == INT_MAX) { *return_code = kIOReturnNoResources; return KERN_SUCCESS; }
		*retainCnt = ++held->refcount;
	}
	else if (action == kIOPMAssertionMIGDoRelease)
	{
		*retainCnt = --held->refcount;
		if (!held->refcount) app->killAssertion(assertion_id);
	}
	else { *return_code = kIOReturnBadArgument; return KERN_SUCCESS; }
	*return_code = kIOReturnSuccess;
	return KERN_SUCCESS;
}

kern_return_t _io_pm_assertion_set_properties(mach_port_t server, audit_token_t token,
	int assertion_id, vm_offset_t props, mach_msg_type_number_t propsCnt,
	int* disableAppSleep, int* enableAppSleep, int* enTrIntensity, int* return_code)
{
	*disableAppSleep = *enableAppSleep = 0;
	*enTrIntensity = -1;
	*return_code = kIOReturnBadArgument;
	CFDictionaryRef properties = copyProperties(props, propsCnt);
	if (!properties) return KERN_SUCCESS;
	bool enabled = true;
	if (CFDictionaryGetCount(properties) != 1 || !CFDictionaryContainsKey(properties, kIOPMAssertionLevelKey))
		*return_code = kIOReturnUnsupported;
	else if (assertionLevel(properties, &enabled))
	{
		pid_t callerPid;
		audit_token_to_au32(token, nullptr, nullptr, nullptr, nullptr, nullptr, &callerPid, nullptr, nullptr);
		AppAssertions* app = AppAssertions::get(callerPid, false);
		auto* held = app ? app->getAssertion(assertion_id) : nullptr;
		*return_code = held ? app->setEnabled(held, enabled) : kIOReturnNotFound;
	}
	CFRelease(properties);
	return KERN_SUCCESS;
}
