#pragma once

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Requests SITE hands to the main loop: power and app jumps run there, after the reply went out. */
typedef enum { SITE_IDLE, SITE_REBOOT, SITE_POWEROFF, SITE_LAUNCH, SITE_PLAY } site_req_t;

/* Registers the SITE verbs and the STOR sink with ftp.c; call before ftp_start.
 * app: the launcher (LAUNCH, CIA install), else the plugin (PLAY, install refused). */
void site_init(bool app);
/* Takes the pending request, if any. title and media are set for SITE_LAUNCH. */
site_req_t site_take(u64 *title, u8 *media);
/* REBOOT or POWEROFF through ptm:sysm. Returns only if it failed. */
void site_power(site_req_t req);

#ifdef __cplusplus
}
#endif
