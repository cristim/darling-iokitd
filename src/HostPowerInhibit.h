/* Host power-save inhibitors. GPL-3.0-or-later, like iokitd. */
#ifndef IOKITD_HOST_POWER_INHIBIT_H
#define IOKITD_HOST_POWER_INHIBIT_H
#include "LogindSleep.h"
#ifdef __cplusplus
extern "C" {
#endif
struct iokitd_power_inhibit;
/* display=0 prevents automatic system idle sleep through logind. display=1
 * additionally inhibits the desktop ScreenSaver; it fails if that service is
 * unavailable, allowing the caller to try a real X11 display backend. */
int iokitd_power_inhibit_create(const struct iokitd_logind_api* api, int display,
                              struct iokitd_power_inhibit** result);
void iokitd_power_inhibit_destroy(struct iokitd_power_inhibit* inhibitor);
#ifdef __cplusplus
}
#endif
#endif
