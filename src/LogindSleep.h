/*
 This file is part of Darling.

 Copyright (C) 2026 Darling developers

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

/*
 * Follows host suspend/resume through systemd-logind on the system bus.
 *
 * At start a "delay" sleep inhibitor is taken. When logind announces a
 * suspend (PrepareForSleep(true)), will_sleep() is called and the inhibitor
 * is kept until iokitd_logind_sleep_ready() is called for that cycle or the
 * acknowledgement timeout expires, whichever comes first; then it is closed
 * so the suspend can proceed. After resume (PrepareForSleep(false)),
 * did_wake() is called and a new inhibitor is taken.
 *
 * The bus is owned by one background thread; the callbacks run on it.
 * iokitd_logind_sleep_ready() may be called from any thread.
 *
 * This part has no Darling dependencies: every host function comes from the
 * table below, so it can also be built natively on Linux against libsystemd
 * for testing. Errors are negative Linux errno values as returned by sd-bus.
 */

#ifndef IOKITD_LOGIND_SLEEP_H_
#define IOKITD_LOGIND_SLEEP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux values; the guest's numbering differs. */
#define IOKITD_LINUX_EINVAL 22
#define IOKITD_LINUX_ENOMEM 12
#define IOKITD_LINUX_ENOSYS 38
#define IOKITD_LINUX_EINTR 4
#define IOKITD_LINUX_F_DUPFD_CLOEXEC 1030

#define IOKITD_LOGIND_DEFAULT_SYSTEM_BUS "unix:path=/run/dbus/system_bus_socket"

/* Layout of sd_bus_error. */
struct iokitd_bus_error
{
	const char* name;
	const char* message;
	int _need_free;
};

typedef int (*iokitd_bus_handler)(void* m, void* userdata, struct iokitd_bus_error* ret_error);

struct iokitd_logind_api
{
	/* libsystemd (sd-bus); bus, message and slot objects are opaque */
	int (*sd_bus_new)(void** ret);
	int (*sd_bus_set_address)(void* bus, const char* address);
	int (*sd_bus_set_bus_client)(void* bus, int b);
	int (*sd_bus_start)(void* bus);
	int (*sd_bus_set_method_call_timeout)(void* bus, uint64_t timeout);
	void* (*sd_bus_flush_close_unref)(void* bus);
	int (*sd_bus_match_signal)(void* bus, void** ret_slot, const char* sender, const char* path,
		const char* interface, const char* member, iokitd_bus_handler callback, void* userdata);
	int (*sd_bus_call_method)(void* bus, const char* destination, const char* path,
		const char* interface, const char* member, struct iokitd_bus_error* ret_error,
		void** reply, const char* types, ...);
	int (*sd_bus_get_property_trivial)(void* bus, const char* destination, const char* path,
		const char* interface, const char* member, struct iokitd_bus_error* ret_error,
		char type, void* ret_ptr);
	int (*sd_bus_message_read)(void* m, const char* types, ...);
	void* (*sd_bus_message_unref)(void* m);
	void* (*sd_bus_slot_unref)(void* slot);
	void (*sd_bus_error_free)(struct iokitd_bus_error* e);
	int (*sd_bus_process)(void* bus, void** ret);
	int (*sd_bus_wait)(void* bus, uint64_t timeout_usec);

	/* Host file descriptors (the inhibitor lock) */
	int (*fcntl)(int fd, int cmd, ...);
	int (*close)(int fd);

	/* Process environment (DBUS_SYSTEM_BUS_ADDRESS) */
	char* (*getenv)(const char* name);

	/* Optional diagnostics */
	void (*log)(const char* message);

	/*
	 * How long to wait for applications to acknowledge a suspend before
	 * releasing the inhibitor anyway; 0 derives it from logind's
	 * InhibitDelayMaxUSec (one second less, so the release always comes
	 * before logind gives up on us).
	 */
	uint64_t ack_timeout_usec;
};

struct iokitd_logind_callbacks
{
	/* Suspend is imminent. Call iokitd_logind_sleep_ready(cycle) once clients agreed. */
	void (*will_sleep)(void* context, uint64_t cycle);
	/* The system has resumed (or the suspend failed). */
	void (*did_wake)(void* context, uint64_t cycle);
};

struct iokitd_logind;

/*
 * Connects to the system bus (DBUS_SYSTEM_BUS_ADDRESS or the standard
 * socket), subscribes to PrepareForSleep, takes the delay inhibitor and
 * starts the bus thread. A missing logind is not an error: no events arrive
 * then. Returns 0 or a negative errno (no bus, incomplete api).
 */
int iokitd_logind_start(const struct iokitd_logind_api* api,
	const struct iokitd_logind_callbacks* callbacks, void* context,
	struct iokitd_logind** ret);

/* Releases the inhibitor if it is still held for this suspend cycle. */
void iokitd_logind_sleep_ready(struct iokitd_logind* logind, uint64_t cycle);

/* Acknowledgement timeout in use, in microseconds. */
uint64_t iokitd_logind_ack_timeout_usec(const struct iokitd_logind* logind);

/* 1 if a delay inhibitor is currently held. */
int iokitd_logind_holds_inhibitor(struct iokitd_logind* logind);

/* Stops the bus thread, closes the inhibitor and the bus. */
void iokitd_logind_stop(struct iokitd_logind* logind);

#ifdef __cplusplus
}
#endif

#endif
