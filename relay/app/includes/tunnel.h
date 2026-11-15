#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reverse tunnel to the broker (relay/tunnel.py): the console dials out over TLS so FTP and SITE
 * reach it from the broker's address. Off unless sdmc:/3ds/relay/tunnel exists. socInit first. */
void tunnel_start(int prio);
void tunnel_stop(void);

enum { TUNNEL_OFF, TUNNEL_DIALING, TUNNEL_UP, TUNNEL_WAIT };
/* State, with a short line for the top screen in out ("Tunnel up", "Tunnel retry 8 s"). */
int tunnel_status(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
