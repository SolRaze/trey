#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftp.h"
#include "knave.hpp"
#include "net.hpp"
#include "update.hpp"

// Percent-encodes everything but unreserved characters, so names with spaces survive the URL.
static void escape(char* out, size_t cap, const char* in) {
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~", c))
            out[n++] = c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

bool installInbox(char* msg, size_t cap) {
    u32 len = 0, status = 0;
    u8* list = netGetAll(KNAVE "inbox", &len, &status, msg, cap);
    if (!list) return false;
    int done = 0, failed = 0;
    for (char* line = strtok((char*)list, "\n"); line; line = strtok(nullptr, "\n")) {
        char* tab = strchr(line, '\t');
        if (tab) *tab = 0;
        char url[512], name[384], why[96];
        escape(name, sizeof(name), line);
        snprintf(url, sizeof(url), KNAVE "inbox/%s", name);
        if (!installCia(url, why, sizeof(why))) { ftp_log("inbox %s: %s", line, why); failed++; continue; }
        strncat(url, "/done", sizeof(url) - strlen(url) - 1);
        netSend("POST", url, "", 0, why, sizeof(why));
        ftp_log("inbox installed %s", line);
        done++;
    }
    free(list);
    if (!done && !failed) snprintf(msg, cap, "Inbox empty");
    else snprintf(msg, cap, "%d installed%s", done, failed ? ", some failed, see log" : "");
    return !failed;
}

void wakeAzahar() {
    char msg[96];
    if (!netSend("POST", KNAVE "azahar", "", 0, msg, sizeof(msg))) ftp_log("azahar wake: %s", msg);
}
