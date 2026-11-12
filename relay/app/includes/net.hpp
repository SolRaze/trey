#pragma once

#include <3ds.h>
#include <stddef.h>

// libcurl over the app's sockets, mbedTLS, roots from certs/cacert.pem (the Mac's /etc/ssl/cert.pem).
// The console's own httpc TLS fails GitHub's handshake (D8A0A03C). Needs socInit first; netInit once.
bool netInit();
void netExit();

// Whole body of a GET into a new malloc'd buffer, NUL-terminated past *len. NULL with msg set on any
// failure; *status gets the final status after redirects (404 tells "none there" from a fault).
u8* netGetAll(const char* url, u32* len, u32* status, char* msg, size_t cap);

// GET streamed to sink in order; sink returns false to stop. True when the whole body arrived with 200.
bool netStream(const char* url, bool (*sink)(void* ctx, const void* p, size_t n), void* ctx, char* msg, size_t cap);
