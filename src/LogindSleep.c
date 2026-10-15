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

#include "LogindSleep.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOGIND_NAME "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER "org.freedesktop.login1.Manager"

#define USEC_PER_SEC 1000000ULL
/* logind's default InhibitDelayMaxSec */
#define DEFAULT_LOGIND_DELAY_USEC (5 * USEC_PER_SEC)
/* What macOS gives applications to answer kIOMessageSystemWillSleep */
#define MAX_ACK_TIMEOUT_USEC (30 * USEC_PER_SEC)
#define MIN_ACK_TIMEOUT_USEC (USEC_PER_SEC / 10)
/* Upper bound on how long the bus thread sleeps before checking deadlines and stop */
#define POLL_INTERVAL_USEC (200 * 1000ULL)
#define CALL_TIMEOUT_USEC (5 * USEC_PER_SEC)

struct iokitd_logind
{
	struct iokitd_logind_api api;
	struct iokitd_logind_callbacks callbacks;
	void* context;

	void* bus;
	void* slot;
	void* owner_slot;
	char* address;
	pthread_t thread;
	int thread_started;
	uint64_t ack_timeout_usec;

	/* Shared with iokitd_logind_sleep_ready() */
	pthread_mutex_t lock;
	int inhibitor_fd;
	int sleeping;
	uint64_t cycle;
	uint64_t deadline_usec;
	int stop;

	/* Bus thread only */
	int want_inhibitor;
};

static uint64_t now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * USEC_PER_SEC + (uint64_t) ts.tv_nsec / 1000;
}

static void log_message(struct iokitd_logind* l, const char* format, ...)
	__attribute__((format(printf, 2, 3)));

static void log_message(struct iokitd_logind* l, const char* format, ...)
{
	char buffer[256];
	va_list ap;

	if (l->api.log == NULL)
		return;
	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);
	l->api.log(buffer);
}

static int api_complete(const struct iokitd_logind_api* api)
{
	return api != NULL && api->sd_bus_new && api->sd_bus_set_address && api->sd_bus_set_bus_client
		&& api->sd_bus_start && api->sd_bus_set_method_call_timeout && api->sd_bus_flush_close_unref && api->sd_bus_match_signal
		&& api->sd_bus_call_method && api->sd_bus_get_property_trivial && api->sd_bus_message_read
		&& api->sd_bus_message_unref && api->sd_bus_slot_unref && api->sd_bus_error_free
		&& api->sd_bus_process && api->sd_bus_wait && api->fcntl && api->close && api->getenv;
}

/* Must hold l->lock. */
static void close_inhibitor_locked(struct iokitd_logind* l)
{
	if (l->inhibitor_fd >= 0)
	{
		l->api.close(l->inhibitor_fd);
		l->inhibitor_fd = -1;
	}
}

/* Bus thread (or start, before the thread exists). */
static void take_inhibitor(struct iokitd_logind* l)
{
	struct iokitd_bus_error error = { NULL, NULL, 0 };
	void* reply = NULL;
	int fd = -1, own = -1, r;

	r = l->api.sd_bus_call_method(l->bus, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER, "Inhibit",
		&error, &reply, "ssss", "sleep", "Darling",
		"Notify macOS applications before the system sleeps", "delay");
	if (r >= 0)
		r = l->api.sd_bus_message_read(reply, "h", &fd);
	/* The descriptor belongs to the reply message; keep our own copy. */
	if (r >= 0)
	{
		own = l->api.fcntl(fd, IOKITD_LINUX_F_DUPFD_CLOEXEC, 3);
		if (own < 0)
			r = -IOKITD_LINUX_ENOMEM;
	}
	if (r < 0)
		log_message(l, "logind: cannot take a sleep delay lock: %s (%d)",
			error.message ? error.message : "no reply", r);

	if (reply != NULL)
		l->api.sd_bus_message_unref(reply);
	l->api.sd_bus_error_free(&error);

	if (own >= 0)
	{
		pthread_mutex_lock(&l->lock);
		close_inhibitor_locked(l);
		l->inhibitor_fd = own;
		pthread_mutex_unlock(&l->lock);
	}
}

static int on_prepare_for_sleep(void* m, void* userdata, struct iokitd_bus_error* ret_error)
{
	struct iokitd_logind* l = userdata;
	int start = 0;
	uint64_t cycle;

	(void) ret_error;
	if (l->api.sd_bus_message_read(m, "b", &start) < 0)
		return 0;

	pthread_mutex_lock(&l->lock);
	if (start)
	{
		l->cycle++;
		l->sleeping = 1;
		l->deadline_usec = now_usec() + l->ack_timeout_usec;
	}
	else
	{
		l->sleeping = 0;
		/* If logind suspended without waiting for us, the old lock is still valid. */
		l->want_inhibitor = l->inhibitor_fd < 0;
	}
	cycle = l->cycle;
	pthread_mutex_unlock(&l->lock);

	if (start)
	{
		if (l->callbacks.will_sleep)
			l->callbacks.will_sleep(l->context, cycle);
	}
	else if (l->callbacks.did_wake)
		l->callbacks.did_wake(l->context, cycle);
	return 0;
}

/* logind may appear after iokitd starts, or restart while we are awake. */
static int on_logind_owner_changed(void* m, void* userdata, struct iokitd_bus_error* error)
{
	struct iokitd_logind* l = userdata;
	const char *name, *old_owner, *new_owner;
	(void) error;
	if (l->api.sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0
		|| strcmp(name, LOGIND_NAME) != 0)
		return 0;
	pthread_mutex_lock(&l->lock);
	close_inhibitor_locked(l);
	l->sleeping = 0;
	l->want_inhibitor = new_owner[0] != '\0';
	pthread_mutex_unlock(&l->lock);
	return 0;
}

static void close_bus(struct iokitd_logind* l)
{
	if (l->slot) l->api.sd_bus_slot_unref(l->slot);
	if (l->owner_slot) l->api.sd_bus_slot_unref(l->owner_slot);
	if (l->bus) l->api.sd_bus_flush_close_unref(l->bus);
	l->slot = l->owner_slot = l->bus = NULL;
}

static int connect_bus(struct iokitd_logind* l)
{
	int r;
	r = l->api.sd_bus_new(&l->bus);
	if (r >= 0)
		r = l->api.sd_bus_set_address(l->bus, l->address);
	if (r >= 0)
		r = l->api.sd_bus_set_bus_client(l->bus, 1);
	if (r >= 0)
		r = l->api.sd_bus_set_method_call_timeout(l->bus, CALL_TIMEOUT_USEC);
	if (r >= 0)
		r = l->api.sd_bus_start(l->bus);
	/* Subscribe before taking the lock so no announcement is missed. */
	if (r >= 0)
		r = l->api.sd_bus_match_signal(l->bus, &l->slot, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER,
			"PrepareForSleep", on_prepare_for_sleep, l);
	if (r >= 0)
		r = l->api.sd_bus_match_signal(l->bus, &l->owner_slot, "org.freedesktop.DBus",
			"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
			on_logind_owner_changed, l);
	if (r < 0)
		close_bus(l);
	return r;
}

static void connection_lost(struct iokitd_logind* l)
{
	pthread_mutex_lock(&l->lock);
	close_inhibitor_locked(l);
	l->sleeping = 0;
	l->want_inhibitor = 0;
	pthread_mutex_unlock(&l->lock);
	close_bus(l);
}

static void* bus_thread(void* arg)
{
	struct iokitd_logind* l = arg;
	uint64_t next_reconnect = 0, backoff = USEC_PER_SEC;

	for (;;)
	{
		uint64_t wait = POLL_INTERVAL_USEC, now;
		int r;

		pthread_mutex_lock(&l->lock);
		if (l->stop)
		{
			pthread_mutex_unlock(&l->lock);
			break;
		}
		pthread_mutex_unlock(&l->lock);

		if (!l->bus)
		{
			now = now_usec();
			if (now < next_reconnect)
			{
				struct timespec pause = {0, POLL_INTERVAL_USEC * 1000};
				nanosleep(&pause, NULL);
				continue;
			}
			r = connect_bus(l);
			if (r < 0)
			{
				log_message(l, "logind: reconnect failed (%d), retrying", r);
				next_reconnect = now_usec() + backoff;
				backoff = backoff < 15 * USEC_PER_SEC ? backoff * 2 : 30 * USEC_PER_SEC;
				continue;
			}
			backoff = USEC_PER_SEC;
			take_inhibitor(l);
			log_message(l, "logind: system bus reconnected");
		}

		r = l->api.sd_bus_process(l->bus, NULL);
		if (r < 0)
		{
			log_message(l, "logind: system bus connection lost (%d), reconnecting", r);
			connection_lost(l);
			next_reconnect = 0;
			continue;
		}
		if (r > 0)
			continue;

		if (l->want_inhibitor)
		{
			l->want_inhibitor = 0;
			take_inhibitor(l);
		}

		now = now_usec();
		pthread_mutex_lock(&l->lock);
		if (l->sleeping && l->inhibitor_fd >= 0)
		{
			if (now >= l->deadline_usec)
			{
				log_message(l, "logind: applications did not acknowledge sleep in time");
				close_inhibitor_locked(l);
			}
			else if (l->deadline_usec - now < wait)
				wait = l->deadline_usec - now;
		}
		pthread_mutex_unlock(&l->lock);

		r = l->api.sd_bus_wait(l->bus, wait);
		if (r < 0 && r != -IOKITD_LINUX_EINTR)
		{
			log_message(l, "logind: waiting on the system bus failed (%d), reconnecting", r);
			connection_lost(l);
			next_reconnect = 0;
		}
	}

	/* Never hold up a suspend once nobody is listening. */
	pthread_mutex_lock(&l->lock);
	close_inhibitor_locked(l);
	pthread_mutex_unlock(&l->lock);
	return NULL;
}

static uint64_t derive_ack_timeout(struct iokitd_logind* l)
{
	struct iokitd_bus_error error = { NULL, NULL, 0 };
	uint64_t max_delay = 0, timeout;
	int r;

	if (l->api.ack_timeout_usec)
		return l->api.ack_timeout_usec;

	r = l->api.sd_bus_get_property_trivial(l->bus, LOGIND_NAME, LOGIND_PATH, LOGIND_MANAGER,
		"InhibitDelayMaxUSec", &error, 't', &max_delay);
	l->api.sd_bus_error_free(&error);
	if (r < 0 || max_delay == 0)
		max_delay = DEFAULT_LOGIND_DELAY_USEC;

	timeout = max_delay > 2 * USEC_PER_SEC ? max_delay - USEC_PER_SEC : max_delay / 2;
	if (timeout > MAX_ACK_TIMEOUT_USEC)
		timeout = MAX_ACK_TIMEOUT_USEC;
	if (timeout < MIN_ACK_TIMEOUT_USEC)
		timeout = MIN_ACK_TIMEOUT_USEC;
	return timeout;
}

static void destroy(struct iokitd_logind* l)
{
	close_bus(l);
	free(l->address);
	// No bus thread exists here (startup failure or stop has joined it).
	close_inhibitor_locked(l);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

int iokitd_logind_start(const struct iokitd_logind_api* api,
	const struct iokitd_logind_callbacks* callbacks, void* context,
	struct iokitd_logind** ret)
{
	struct iokitd_logind* l;
	const char* address;
	int r;

	*ret = NULL;
	if (!api_complete(api) || callbacks == NULL)
		return -IOKITD_LINUX_ENOSYS;

	l = calloc(1, sizeof(*l));
	if (l == NULL)
		return -IOKITD_LINUX_ENOMEM;
	l->api = *api;
	l->callbacks = *callbacks;
	l->context = context;
	l->inhibitor_fd = -1;
	// Signal callbacks can run while synchronous startup calls pump the bus.
	l->ack_timeout_usec = api->ack_timeout_usec ? api->ack_timeout_usec
		: DEFAULT_LOGIND_DELAY_USEC - USEC_PER_SEC;
	pthread_mutex_init(&l->lock, NULL);

	address = api->getenv("DBUS_SYSTEM_BUS_ADDRESS");
	if (address == NULL || address[0] == '\0')
		address = IOKITD_LOGIND_DEFAULT_SYSTEM_BUS;

	l->address = strdup(address);
	if (!l->address)
	{
		destroy(l);
		return -IOKITD_LINUX_ENOMEM;
	}
	r = connect_bus(l);
	if (r < 0)
	{
		log_message(l, "logind: cannot use the system bus at %s (%d)", address, r);
		destroy(l);
		return r;
	}

	l->ack_timeout_usec = derive_ack_timeout(l);
	take_inhibitor(l);

	r = pthread_create(&l->thread, NULL, bus_thread, l);
	if (r != 0)
	{
		destroy(l);
		return -IOKITD_LINUX_ENOMEM;
	}
	l->thread_started = 1;
	*ret = l;
	return 0;
}

void iokitd_logind_sleep_ready(struct iokitd_logind* l, uint64_t cycle)
{
	if (l == NULL)
		return;
	pthread_mutex_lock(&l->lock);
	if (l->sleeping && l->cycle == cycle)
		close_inhibitor_locked(l);
	pthread_mutex_unlock(&l->lock);
}

uint64_t iokitd_logind_ack_timeout_usec(const struct iokitd_logind* l)
{
	return l ? l->ack_timeout_usec : 0;
}

int iokitd_logind_holds_inhibitor(struct iokitd_logind* l)
{
	int held;

	if (l == NULL)
		return 0;
	pthread_mutex_lock(&l->lock);
	held = l->inhibitor_fd >= 0;
	pthread_mutex_unlock(&l->lock);
	return held;
}

void iokitd_logind_stop(struct iokitd_logind* l)
{
	if (l == NULL)
		return;
	pthread_mutex_lock(&l->lock);
	l->stop = 1;
	pthread_mutex_unlock(&l->lock);
	if (l->thread_started)
		pthread_join(l->thread, NULL);
	destroy(l);
}
