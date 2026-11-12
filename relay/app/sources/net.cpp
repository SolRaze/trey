#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "cacert_pem.h"
#include "ftp.h"
#include "net.hpp"

bool netInit() { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }
void netExit() { curl_global_cleanup(); }

// "knave" for ours, else the host name, for screen messages.
static void hostOf(const char* url, char* out, size_t cap) {
    if (!strncmp(url, KNAVE, strlen(KNAVE))) { snprintf(out, cap, "knave"); return; }
    const char* h = strstr(url, "://");
    h = h ? h + 3 : url;
    snprintf(out, cap, "%.*s", (int)strcspn(h, "/:"), h);
}

static CURL* easy(const char* url) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "relay/" RELAY_BUILD);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);     // a stalled transfer ends after 20 s
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    struct curl_blob ca = { (void*)cacert_pem, cacert_pem_size, CURL_BLOB_NOCOPY };
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
    return c;
}

// Runs c; true on 200. msg names the host and what went wrong otherwise.
static bool perform(CURL* c, const char* url, u32* status, char* msg, size_t cap) {
    char host[64];
    hostOf(url, host, sizeof(host));
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    if (r != CURLE_OK && r != CURLE_WRITE_ERROR) {
        snprintf(msg, cap, "Cannot reach %s (%s)", host, curl_easy_strerror(r));
        ftp_log("http %s: %s", host, curl_easy_strerror(r));
        return false;
    }
    if (code != 200) { snprintf(msg, cap, "%s answered %ld", host, code); return false; }
    if (r == CURLE_WRITE_ERROR) { snprintf(msg, cap, "Stopped while receiving"); return false; }
    return true;
}

struct Grow { u8* p; size_t n, cap; };

static size_t growWrite(char* data, size_t sz, size_t nm, void* ud) {
    Grow* g = (Grow*)ud;
    size_t n = sz * nm;
    if (g->n + n + 1 > g->cap) {
        size_t cap = (g->n + n + 1) * 2;
        u8* p = (u8*)realloc(g->p, cap);
        if (!p) return 0;
        g->p = p; g->cap = cap;
    }
    memcpy(g->p + g->n, data, n);
    g->n += n;
    return n;
}

u8* netGetAll(const char* url, u32* len, u32* status, char* msg, size_t cap) {
    *len = 0; *status = 0;
    CURL* c = easy(url);
    if (!c) { snprintf(msg, cap, "Out of memory"); return nullptr; }
    Grow g = { nullptr, 0, 0 };
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, growWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);
    bool ok = perform(c, url, status, msg, cap);
    curl_easy_cleanup(c);
    if (!ok) { free(g.p); return nullptr; }
    if (!g.p) g.p = (u8*)malloc(1);
    if (!g.p) { snprintf(msg, cap, "Out of memory"); return nullptr; }
    g.p[g.n] = 0;
    *len = g.n;
    return g.p;
}

struct Sink { bool (*fn)(void*, const void*, size_t); void* ctx; };

static size_t sinkWrite(char* data, size_t sz, size_t nm, void* ud) {
    Sink* s = (Sink*)ud;
    return s->fn(s->ctx, data, sz * nm) ? sz * nm : 0;
}

bool netStream(const char* url, bool (*sink)(void*, const void*, size_t), void* ctx, char* msg, size_t cap) {
    CURL* c = easy(url);
    if (!c) { snprintf(msg, cap, "Out of memory"); return false; }
    Sink s = { sink, ctx };
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sinkWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    // curl fails a body shorter than its Content-Length (CURLE_PARTIAL_FILE), so ok means complete.
    bool ok = perform(c, url, nullptr, msg, cap);
    curl_easy_cleanup(c);
    return ok;
}

static size_t discard(char*, size_t sz, size_t nm, void*) { return sz * nm; }

bool netSend(const char* method, const char* url, const void* body, u32 len, char* msg, size_t cap) {
    CURL* c = easy(url);
    if (!c) { snprintf(msg, cap, "Out of memory"); return false; }
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)len);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
    struct curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/octet-stream");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    bool ok = perform(c, url, nullptr, msg, cap);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return ok;
}
