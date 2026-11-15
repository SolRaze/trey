/* Shared by the core sysmodule (core/) and the Relay app, which installs and launches it. */
#pragma once
#include <3ds.h>

#define CORE_TID      0x0004013000F0E702ULL
#define CORE_CXI      "/luma/sysmodules/0004013000F0E702.cxi"
#define CORE_NAME     "rcore"                  /* codeset name, core.rsf Title */
#define CORE_LOG      "/3ds/relay/core.log"    /* events, trimmed to the last 32 KB at start */
#define CORE_STATUS   "/3ds/relay/core.status" /* rewritten every 5 s, "key value" lines */
#define CORE_RUN      "/3ds/relay/core.run"    /* "<boot id>", present while the core runs */
#define CORE_LAUNCHES "/3ds/relay/core.boot"   /* "<boot id> <launches>", Relay's crash guard */

/* Minutes since 1900 at power-on: the same for every process during one boot.
 * Moves if the RTC is set mid-boot (Relay's clock sync), which reads as a new boot. */
static inline u32 core_boot_id(void) {
    u64 up_ms = svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
    return (u32)((osGetTime() - up_ms) / 60000);
}
