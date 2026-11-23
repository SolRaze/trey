/* Shared by the sysmodules (core/, tsmod/) and the Relay app, which installs and launches them. */
#pragma once
#include <3ds.h>
#include <string.h>

#define CORE_TID      0x0004013000F0E702ULL
#define CORE_CXI      "/luma/sysmodules/0004013000F0E702.cxi"
#define CORE_NAME     "rcore"                  /* codeset name, core.rsf Title */
#define CORE_LOG      "/3ds/relay/core.log"    /* events, trimmed to the last 32 KB at start */
#define CORE_STATUS   "/3ds/relay/core.status" /* rewritten every 5 s, "key value" lines */
#define CORE_RUN      "/3ds/relay/core.run"    /* "<boot id>", present while the core runs */
#define CORE_LAUNCHES "/3ds/relay/core.boot"   /* "<boot id> <launches>", Relay's crash guard */

/* Tailscale node sysmodule, system memory region (too big for base). Same launch rules as the core. */
#define TS_TID        0x0004013000F0E802ULL
#define TS_CXI        "/luma/sysmodules/0004013000F0E802.cxi"
#define TS_NAME       "rts"                    /* codeset name, tsmod.rsf Title */
#define TS_DIR        "/3ds/relay/ts"          /* ts.log, node store */
#define TS_KEY        "/3ds/relay/tskey"       /* auth key; absent = node off */
#define TS_STATUS     TS_DIR "/status"         /* "<0|1> <line>", 1 = on the tailnet; rewritten every 2 s */
#define TS_LAUNCHES   TS_DIR "/boot"           /* "<boot id> <launches>", Relay's crash guard */
#define TS_STOP       TS_DIR "/stop"           /* present = the module exits within 2 s, deleting it */

/* Minutes since 1900 at power-on: the same for every process during one boot.
 * Moves if the RTC is set mid-boot (Relay's clock sync), which reads as a new boot. */
static inline u32 core_boot_id(void) {
    u64 up_ms = svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
    return (u32)((osGetTime() - up_ms) / 60000);
}

/* Process names come from Luma's svcGetProcessInfo extension (type 0x10000).
 * Needs svcGetProcessList/svcOpenProcess rights: Relay has them, a game process may not. */
static inline bool proc_pid(const char *want, u32 *pid) {
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64))) return false;
    for (s32 i = 0; i < n; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) continue;
        char name[9] = {0};
        svcGetProcessInfo((s64 *)name, h, 0x10000);
        svcCloseHandle(h);
        if (!strncmp(name, want, 8)) { if (pid) *pid = pids[i]; return true; }
    }
    return false;
}

static inline bool core_pid(u32 *pid) { return proc_pid(CORE_NAME, pid); }
static inline bool core_running(void) { return proc_pid(CORE_NAME, NULL); }
