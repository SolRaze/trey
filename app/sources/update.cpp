#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "ftp.h"
#include "net.hpp"
#include "site.h"
#include "update.hpp"

// Build ids are "<commit>[+]@<yymmddhhmm>" (top Makefile BUILD); the stamp after '@' orders them, so an
// FTP install that runs ahead of the release is never stepped back.
static unsigned long long stamp(const char* id) {
    const char* at = strrchr(id, '@');
    return at ? strtoull(at + 1, nullptr, 10) : 0;
}

static char remote[64];

UpdateResult checkUpdate(char* msg, size_t cap) {
    u32 len = 0, status = 0;
    u8* b = netGetAll(UPDATE_BASE "build", &len, &status, msg, cap);
    if (!b) return UpdateResult::Failed;
    b[strcspn((char*)b, "\r\n")] = 0;
    snprintf(remote, sizeof(remote), "%s", (char*)b);
    free(b);
    if (stamp(remote) <= stamp(RELAY_BUILD)) {
        snprintf(msg, cap, "Up to date, %s", RELAY_BUILD);
        return UpdateResult::Current;
    }
    snprintf(msg, cap, "Update available, %s", remote);
    return UpdateResult::Newer;
}

// AM's CIA file fails writes at unaligned offsets (curl hands over odd-sized pieces, the first ending at
// 16070), so the body is gathered into whole 64 KB writes; only the last one is short.
struct CiaBuf { void* cia; u8* buf; size_t n; };
static const size_t CIA_CHUNK = 0x10000;

static bool ciaSink(void* ud, const void* p, size_t n) {
    CiaBuf* b = (CiaBuf*)ud;
    const u8* src = (const u8*)p;
    while (n) {
        size_t take = CIA_CHUNK - b->n < n ? CIA_CHUNK - b->n : n;
        memcpy(b->buf + b->n, src, take);
        b->n += take; src += take; n -= take;
        if (b->n == CIA_CHUNK) {
            if (!cia_write(b->cia, b->buf, b->n)) return false;
            b->n = 0;
        }
    }
    return true;
}

bool installCia(const char* url, char* msg, size_t cap) {
    CiaBuf b = { nullptr, (u8*)malloc(CIA_CHUNK), 0 };
    if (!b.buf) { snprintf(msg, cap, "Out of memory"); return false; }
    const char* err = nullptr;
    b.cia = cia_begin(&err);
    if (!b.cia) { free(b.buf); snprintf(msg, cap, "%s", err); return false; }
    bool ok = netStream(url, ciaSink, &b, msg, cap);   // never commits a partial CIA
    if (ok && b.n && !cia_write(b.cia, b.buf, b.n)) { ok = false; snprintf(msg, cap, "Install write failed"); }
    free(b.buf);
    err = cia_end(b.cia, ok);
    if (ok && err) snprintf(msg, cap, "%s", err);
    return ok && !err;
}

UpdateResult runUpdate(char* msg, size_t cap) {
    UpdateResult c = checkUpdate(msg, cap);
    if (c != UpdateResult::Newer) return c;
    if (!installCia(UPDATE_BASE "relay.cia", msg, cap)) {
        ftp_log("update %s failed: %s", remote, msg);
        return UpdateResult::Failed;
    }
    ftp_log("update installed %s", remote);
    snprintf(msg, cap, "Installed %s, restarting", remote);
    return UpdateResult::Installed;
}
