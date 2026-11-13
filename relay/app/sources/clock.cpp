#include <3ds.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clock.hpp"
#include "ftp.h"

// Same steps as Rosalina's NTP option: SNTP from pool.ntp.org, plus Luma's ntp_tz_offset_min from
// /luma/config.ini (the 3DS clock is local time with no zone), written to the RTC with the user time
// offset (config block 0x30001) zeroed, then the config saved. Needs cfg:i or cfg:s and ptm:sysm.

static const long long NTP_TO_Y2K = 3155673600LL;   // seconds, 1900-01-01 to 2000-01-01

bool clockWrong() { return time(NULL) < 1704067200; }

static int tzMinutes() {
    FILE* f = fopen("sdmc:/luma/config.ini", "r");
    if (!f) return 0;
    char line[128];
    int tz = 0;
    while (fgets(line, sizeof(line), f)) if (sscanf(line, " ntp_tz_offset_min = %d", &tz) == 1) break;
    fclose(f);
    return tz;
}

// Milliseconds since 1900 from one SNTP reply, 0 on failure. Socket non-blocking, waits by sleep:
// a blocking call would stall the FTP thread on the shared soc:U session.
static long long ntpNow() {
    struct hostent* h = gethostbyname("pool.ntp.org");
    if (!h || !h->h_addr_list[0]) { ftp_log("clock: pool.ntp.org did not resolve"); return 0; }
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(123);
    memcpy(&a.sin_addr, h->h_addr_list[0], 4);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    u8 pkt[48] = { 0x1B };   // version 3, client
    long long ms = 0;
    if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&a, sizeof(a)) == sizeof(pkt)) {
        for (int i = 0; i < 30 && !ms; i++) {
            svcSleepThread(100000000LL);
            if (recv(s, pkt, sizeof(pkt), 0) < 48) continue;
            u32 sec = (u32)pkt[40] << 24 | pkt[41] << 16 | pkt[42] << 8 | pkt[43];
            u32 frac = (u32)pkt[44] << 24 | pkt[45] << 16 | pkt[46] << 8 | pkt[47];
            if (sec) ms = sec * 1000LL + (((u64)frac * 1000) >> 32);
        }
    }
    close(s);
    if (!ms) ftp_log("clock: no NTP reply");
    return ms;
}

bool clockSync(char* msg, size_t cap) {
    long long ms = ntpNow();
    if (!ms) { snprintf(msg, cap, "No time server reply, check Wi-Fi"); return false; }
    int tz = tzMinutes();
    s64 msY2k = ms - NTP_TO_Y2K * 1000 + tz * 60000LL;
    Result r = cfguInit();
    if (R_SUCCEEDED(r)) {
        u8 zero[8] = { 0 };
        r = CFG_SetConfigInfoBlk4(8, 0x30001, zero);
        if (R_SUCCEEDED(r) && R_SUCCEEDED(r = ptmSysmInit())) {
            r = PTMSYSM_SetRtcTime(msY2k);
            ptmSysmExit();
        }
        if (R_SUCCEEDED(r)) r = CFG_UpdateConfigSavegame();
        cfguExit();
    }
    if (R_FAILED(r)) {
        ftp_log("clock: set failed %08lX", r);
        snprintf(msg, cap, "Could not set the clock, %08lX", r);
        return false;
    }
    time_t t = (time_t)(msY2k / 1000 + 946684800LL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char when[32];
    strftime(when, sizeof(when), "%d %b %Y  %H:%M", &tm);
    int az = tz < 0 ? -tz : tz;
    snprintf(msg, cap, "Clock set to %s, UTC%c%d:%02d", when, tz < 0 ? '-' : '+', az / 60, az % 60);
    ftp_log("clock: %s", msg);
    return true;
}
