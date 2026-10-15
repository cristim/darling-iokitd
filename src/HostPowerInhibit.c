/* Host power-save inhibitors. GPL-3.0-or-later, like iokitd. */
#include "HostPowerInhibit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct iokitd_power_inhibit {
    struct iokitd_logind_api api;
    int idle_fd;
    void* screen_bus;
    uint32_t screen_cookie;
    int screen_held;
};

static int connect_bus(const struct iokitd_logind_api* api, const char* address, void** bus)
{
    int r = api->sd_bus_new(bus);
    if (r >= 0) r = api->sd_bus_set_address(*bus, address);
    if (r >= 0) r = api->sd_bus_set_bus_client(*bus, 1);
    /* Assertions are synchronous MIG calls; do not stall the IOKit queue for
     * sd-bus's default 25-second service timeout. */
    if (r >= 0) r = api->sd_bus_set_method_call_timeout(*bus, 500000);
    if (r >= 0) r = api->sd_bus_start(*bus);
    return r;
}

static int take_idle_lock(struct iokitd_power_inhibit* inhibitor)
{
    const struct iokitd_logind_api* api = &inhibitor->api;
    const char* address = api->getenv("DBUS_SYSTEM_BUS_ADDRESS");
    struct iokitd_bus_error error = {0};
    void *bus = NULL, *reply = NULL;
    int fd = -1;
    if (!address || !address[0]) address = IOKITD_LOGIND_DEFAULT_SYSTEM_BUS;
    int r = connect_bus(api, address, &bus);
    if (r >= 0)
        r = api->sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
                "idle", "Darling", "macOS application prevents automatic idle sleep", "block");
    if (r >= 0) r = api->sd_bus_message_read(reply, "h", &fd);
    if (r >= 0) {
        inhibitor->idle_fd = api->fcntl(fd, IOKITD_LINUX_F_DUPFD_CLOEXEC, 3);
        if (inhibitor->idle_fd < 0) r = -IOKITD_LINUX_ENOMEM;
    }
    if (reply) api->sd_bus_message_unref(reply);
    api->sd_bus_error_free(&error);
    if (bus) api->sd_bus_flush_close_unref(bus);
    return r;
}

static int take_screen_lock(struct iokitd_power_inhibit* inhibitor)
{
    const struct iokitd_logind_api* api = &inhibitor->api;
    const char* address = api->getenv("DBUS_SESSION_BUS_ADDRESS");
    char fallback[512];
    if (!address || !address[0]) {
        const char* runtime = api->getenv("XDG_RUNTIME_DIR");
        int length = runtime && runtime[0]
            ? snprintf(fallback, sizeof(fallback), "unix:path=%s/bus", runtime)
            : snprintf(fallback, sizeof(fallback), "unix:path=/run/user/%u/bus", (unsigned)getuid());
        if (length < 0 || (size_t)length >= sizeof(fallback)) return -IOKITD_LINUX_EINVAL;
        address = fallback;
    }
    struct iokitd_bus_error error = {0};
    void* reply = NULL;
    int r = connect_bus(api, address, &inhibitor->screen_bus);
    if (r >= 0)
        r = api->sd_bus_call_method(inhibitor->screen_bus, "org.freedesktop.ScreenSaver",
                "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "Inhibit",
                &error, &reply, "ss", "Darling", "macOS application prevents display sleep");
    if (r >= 0) r = api->sd_bus_message_read(reply, "u", &inhibitor->screen_cookie);
    if (r >= 0) inhibitor->screen_held = 1;
    if (reply) api->sd_bus_message_unref(reply);
    api->sd_bus_error_free(&error);
    return r;
}

int iokitd_power_inhibit_create(const struct iokitd_logind_api* api, int display,
                              struct iokitd_power_inhibit** result)
{
    *result = NULL;
    if (!api || !api->sd_bus_new || !api->sd_bus_set_address || !api->sd_bus_set_bus_client ||
        !api->sd_bus_set_method_call_timeout || !api->sd_bus_start || !api->sd_bus_call_method ||
        !api->sd_bus_message_read || !api->sd_bus_message_unref || !api->sd_bus_error_free ||
        !api->sd_bus_flush_close_unref || !api->fcntl || !api->close || !api->getenv)
        return -IOKITD_LINUX_ENOSYS;
    struct iokitd_power_inhibit* inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor) return -IOKITD_LINUX_ENOMEM;
    inhibitor->api = *api;
    inhibitor->idle_fd = -1;
    int r = take_idle_lock(inhibitor);
    if (r >= 0 && display) r = take_screen_lock(inhibitor);
    if (r < 0) {
        iokitd_power_inhibit_destroy(inhibitor);
        return r;
    }
    *result = inhibitor;
    return 0;
}

void iokitd_power_inhibit_destroy(struct iokitd_power_inhibit* inhibitor)
{
    if (!inhibitor) return;
    const struct iokitd_logind_api* api = &inhibitor->api;
    if (inhibitor->screen_held) {
        struct iokitd_bus_error error = {0};
        void* reply = NULL;
        api->sd_bus_call_method(inhibitor->screen_bus, "org.freedesktop.ScreenSaver",
                "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "UnInhibit",
                &error, &reply, "u", inhibitor->screen_cookie);
        if (reply) api->sd_bus_message_unref(reply);
        api->sd_bus_error_free(&error);
    }
    if (inhibitor->screen_bus) api->sd_bus_flush_close_unref(inhibitor->screen_bus);
    if (inhibitor->idle_fd >= 0) api->close(inhibitor->idle_fd);
    free(inhibitor);
}
