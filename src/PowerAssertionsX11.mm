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

#include "PowerAssertionsX11.h"
#include "IODisplayConnectX11.h"
#include <iostream>
#include <X11/Xlib.h>

#define BOOL	BOOL_THOU_SHALT_NOT_TYPEDEF_BOOL_IN_UNIMPORTANT_HEADERS
#include <X11/extensions/dpms.h>
#undef BOOL

IOReturn PowerAssertionPreventDisplaySleep::activate()
{
	const auto* api = iokitd_host_power_api();
	if (api && iokitd_power_inhibit_create(api, 1, &m_host) == 0)
		return kIOReturnSuccess;

	// Desktop inhibition is preferred on Wayland; retain the real X11
	// fallback when no ScreenSaver service is available.
	Display* display = IODisplayConnectX11::getDisplay();
	int eventBase, errorBase;
	CARD16 power;
	unsigned char enabled;
	if (!display || !DPMSQueryExtension(display, &eventBase, &errorBase) ||
		!DPMSCapable(display) || !DPMSInfo(display, &power, &enabled))
		return kIOReturnNotReady;
	if (api)
		iokitd_power_inhibit_create(api, 0, &m_host);
	m_changedDPMS = enabled;
	if (m_changedDPMS) DPMSDisable(display);
	XFlush(display);
	return kIOReturnSuccess;
}

void PowerAssertionPreventDisplaySleep::deactivate()
{
	iokitd_power_inhibit_destroy(m_host);
	m_host = nullptr;
	if (m_changedDPMS)
	{
		Display* display = IODisplayConnectX11::getDisplay();
		if (display) { DPMSEnable(display); XFlush(display); }
		m_changedDPMS = false;
	}
}
