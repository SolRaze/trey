/* Relay Tailscale node: a sysmodule in the system memory region that outlives Relay, HOME and games.
 *
 * Runs port/node.c: the tailnet node, plus serve.c bridging 100.x FTP ports to the local FTP server
 * that Relay or the broadcast plugin provides. Writes TS_STATUS for Relay's screen and SITE STAT.
 * Started by Relay through pm:app like the core, never terminated (terminate then relaunch faults the
 * kernel in loader); it exits on its own when TS_STOP appears, which Relay writes for a new build.
 *
 * Constraints: fixed heap of HEAP_SIZE out of the system region (engine peak about 750 KB, plus the
 * soc buffer), charged to the OTHER reslimit, which heap setup raises to fit; ts.log has the figures at start. The main
 * loop blocks on srv notifications; engine threads sleep in every wait. */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build.h"
#include "core.h"
#include "ts_node.h"

#define HEAP_SIZE (2 * 1024 * 1024)
#define HEAP_MIN  (64 * 1024)   /* fallback, enough to report the failure */
#define SOC_SIZE  0x80000
#define TICK_NS   2000000000LL

/* ---- runtime: fixed heap, no APT ---- */

extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

static Result heap_res;

static bool heap_alloc(u32 size) {
    u32 tmp = 0;
    __ctru_heap_size = size;
    return R_SUCCEEDED(heap_res = svcControlMemory(&tmp, __ctru_heap, 0, size, MEMOP_ALLOC,
                                                   MEMPERM_READ | MEMPERM_WRITE));
}

/* The OTHER commit limit is sized for the base region and the base modules fill it, but this heap
 * comes from the system region. Raise the limit to cover it; idempotent, so a relaunch adds nothing. */
static void raise_commit(void) {
    Handle rl;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) return;
    ResourceLimitType t = RESLIMIT_COMMIT;
    s64 cur = 0, lim = 0;
    svcGetResourceLimitCurrentValues(&cur, rl, &t, 1);
    svcGetResourceLimitLimitValues(&lim, rl, &t, 1);
    s64 want = cur + HEAP_SIZE + 0x10000;
    if (want > lim) svcSetResourceLimitValues(rl, &t, &want, 1);
    svcCloseHandle(rl);
}

void __system_allocateHeaps(void) {
    raise_commit();
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    if (!heap_alloc(HEAP_SIZE)) {
        Result full = heap_res;
        if (!heap_alloc(HEAP_MIN)) svcBreak(USERBREAK_PANIC);
        heap_res = full;   /* main sees the failure and exits */
    }
    __ctru_linear_heap = 0;
    __ctru_linear_heap_size = 0;
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

void __appInit(void) {
    /* srv may not be up yet when launched early; 0xD88007FA = port full, retry as Rosalina does */
    Result r;
    while ((r = srvInit()) == (Result)0xD88007FA) svcSleepThread(500 * 1000000LL);
    if (R_FAILED(r)) svcBreak(USERBREAK_PANIC);
    if (R_FAILED(fsInit())) svcBreak(USERBREAK_PANIC);
    archiveMountSdmc();
    chdir("sdmc:/");   /* default device: bare "/3ds/..." paths resolve to sdmc */
}

void __appExit(void) {
    archiveUnmountAll();
    fsExit();
    srvExit();
}

/* ---- log and status ---- */

static void note(const char *fmt, ...) {
    FILE *f = fopen(TS_DIR "/ts.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Rewritten only on change: Relay polls it, the card need not see a write every tick. */
static void status(bool up, const char *line) {
    static char last[64];
    char now[64];
    snprintf(now, sizeof now, "%d %s\n", up, line);
    if (!strcmp(now, last)) return;
    FILE *f = fopen(TS_STATUS ".new", "w");
    if (!f) return;
    fputs(now, f);
    fclose(f);
    remove(TS_STATUS);
    rename(TS_STATUS ".new", TS_STATUS);
    strcpy(last, now);
}

/* Every reslimit of this process's category, "name cur/limit"; the category is shared with HOME. */
static void limits(const char *when) {
    static const char *names[] = { "prio", "commitKB", "thread", "event", "mutex", "sem", "timer",
                                   "shmem", "arbiter", "cpu" };
    ResourceLimitType t[10];
    s64 cur[10] = {0}, lim[10] = {0};
    for (int i = 0; i < 10; i++) t[i] = (ResourceLimitType)i;
    Handle rl;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) { note("%s: no reslimit", when); return; }
    svcGetResourceLimitCurrentValues(cur, rl, t, 10);
    svcGetResourceLimitLimitValues(lim, rl, t, 10);
    svcCloseHandle(rl);
    cur[1] /= 1024;
    lim[1] /= 1024;
    char line[256];
    size_t n = 0;
    for (int i = 0; i < 10 && n < sizeof line; i++)
        n += snprintf(line + n, sizeof line - n, " %s %lld/%lld", names[i], cur[i], lim[i]);
    note("%s:%s", when, line);
}

/* ---- main ---- */

int main(void) {
    mkdir(TS_DIR, 0777);
    status(false, "starting");   /* replaces the last boot's file before Relay reads it */
    note("tsmod %s heap %08lX, system free %lu KB", RELAY_BUILD, heap_res,
         osGetMemRegionFree(MEMREGION_SYSTEM) / 1024);
    limits("limits at start");
    if (R_FAILED(heap_res)) { status(false, "no memory"); return 1; }

    Handle notif;
    if (R_FAILED(srvEnableNotification(&notif))) { status(false, "no notifications"); return 1; }
    srvSubscribe(0x100);   /* termination */

    void *socbuf = memalign(0x1000, SOC_SIZE);
    if (!socbuf || R_FAILED(socInit(socbuf, SOC_SIZE))) { status(false, "soc failed"); return 1; }

    char line[48];
    if (ts_node_start("sdmc:" TS_DIR, "sdmc:" TS_KEY, "trey") != 0) {
        ts_node_line(line, sizeof line);   /* the reason */
        status(false, line);
        socExit();
        return 1;
    }
    limits("limits with engine");
    for (;;) {
        if (svcWaitSynchronization(notif, TICK_NS) == 0) {
            u32 id = 0;
            if (R_SUCCEEDED(srvReceiveNotification(&id)) && id == 0x100) break;
            continue;
        }
        if (remove(TS_STOP) == 0) { note("stop file"); break; }
        bool up = ts_node_line(line, sizeof line);
        status(up, line);
    }
    ts_node_stop();
    socExit();
    status(false, "stopped");
    svcCloseHandle(notif);
    return 0;
}
