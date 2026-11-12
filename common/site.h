#pragma once

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Requests SITE hands to the main loop: power and app jumps run there, after the reply went out. */
typedef enum { SITE_IDLE, SITE_REBOOT, SITE_POWEROFF, SITE_LAUNCH, SITE_PLAY, SITE_SYNC, SITE_INBOX, SITE_UPDATE } site_req_t;

/* Registers the SITE verbs and the STOR sink with ftp.c; call before ftp_start.
 * app: the launcher (LAUNCH, CIA install), else the plugin (PLAY, install refused). */
void site_init(bool app);
/* Takes the pending request, if any. title and media are set for SITE_LAUNCH. */
site_req_t site_take(u64 *title, u8 *media);
/* REBOOT or POWEROFF through ptm:sysm. Returns only if it failed. */
void site_power(site_req_t req);

/* Streams a CIA into the SD installer: begin, write in order, end. end(ok=0) cancels.
 * begin returns NULL and sets err on failure; end returns NULL when installed, else why not. */
void *cia_begin(const char **err);
int cia_write(void *ctx, const void *p, size_t n);
const char *cia_end(void *ctx, int ok);

#ifdef __cplusplus
}
#endif
