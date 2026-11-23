/* Relay core: a sysmodule in the base memory region that outlives Relay, HOME and games.
 *
 * Now: sleep denied while it runs, lid-shut dark (backlights and every LED but Wi-Fi off),
 * docked state, and a status file with the memory, storage and power readout.
 * Started by Relay through pm:app; Luma's loader reads it from CORE_CXI.
 *
 * Constraints: no APT (sysmodules have no applet slot); heap is fixed at HEAP_SIZE out of the
 * base region, which Rosalina and every system module share; nothing may block the
 * notification loop, it answers PTM's sleep query. */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build.h"
#include "core.h"

#define HEAP_SIZE   (256 * 1024)
#define TICK_NS     2000000000LL   /* lid-dark re-assert and power checks */
#define STATUS_TICKS 3             /* status every 6 s */
#define LOG_KEEP    (32 * 1024)

/* ---- runtime: fixed heap, no APT ---- */

extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

void __system_allocateHeaps(void) {
    u32 tmp = 0;
    __ctru_heap_size = HEAP_SIZE;
    __ctru_linear_heap_size = 0;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    svcControlMemory(&tmp, __ctru_heap, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    __ctru_linear_heap = 0;
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

/* ---- log ---- */

static void trim_log(void) {
    struct stat st;
    if (stat(CORE_LOG, &st) != 0 || st.st_size <= LOG_KEEP) return;
    FILE *f = fopen(CORE_LOG, "rb");
    if (!f) return;
    static char keep[LOG_KEEP];
    fseek(f, -LOG_KEEP, SEEK_END);
    size_t n = fread(keep, 1, sizeof(keep), f);
    fclose(f);
    char *start = memchr(keep, '\n', n);
    start = start ? start + 1 : keep;
    if ((f = fopen(CORE_LOG, "wb"))) { fwrite(start, 1, n - (start - keep), f); fclose(f); }
}

static void note(const char *fmt, ...) {
    FILE *f = fopen(CORE_LOG, "a");
    if (!f) return;
    u64 s = svcGetSystemTick() / SYSCLOCK_ARM11;
    fprintf(f, "%5llu:%02llu ", s / 60, s % 60);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- power, lid, LEDs ---- */

static bool lid_shut, docked;
static u8 battery = 0xFF;
static u32 sleep_denied;

static void lid_dark(void) {
    if (R_SUCCEEDED(gspLcdInit())) { GSPLCD_PowerOffAllBacklights(); gspLcdExit(); }
    if (R_FAILED(mcuHwcInit())) return;
    /* PTM sets the power LED again on charger and battery events, hence the re-assert.
     * At 5 % and below PTM blinks it red; that warning stays. */
    if (battery > 5) MCUHWC_SetPowerLedState(LED_OFF);
    MCU_InfoLedPattern blank;
    memset(&blank, 0, sizeof(blank));
    blank.animation.ticksPerFrame = 1;
    blank.animation.numLastFrameRepeats = 0xFF;
    MCUHWC_SetInfoLedPattern(&blank);
    MCUHWC_Set3dLedState(false);
    MCUHWC_SetCameraLedState(false);
    mcuHwcExit();
}

static void lid_light(void) {
    if (R_SUCCEEDED(gspLcdInit())) { GSPLCD_PowerOnAllBacklights(); gspLcdExit(); }
    /* TODO(later): the notification LED pattern cannot be read back, so whatever lit it
     * before the lid closed (Broadcast's blue, HOME notices) stays blank until its owner sets
     * it again. Upgrade: Relay and the plugin ask the core to set it and the core keeps it. */
    if (R_SUCCEEDED(mcuHwcInit())) { MCUHWC_SetPowerLedState(LED_NORMAL); mcuHwcExit(); }
}

static void set_lid(bool shut) {
    if (shut == lid_shut) return;
    lid_shut = shut;
    note("lid %s", shut ? "shut" : "open");
    if (shut) lid_dark(); else lid_light();
}

static void poll_power(void) {
    bool adapter = docked;
    if (R_SUCCEEDED(ptmuInit())) { PTMU_GetAdapterState(&adapter); ptmuExit(); }
    if (adapter != docked) { docked = adapter; note("%s", docked ? "docked" : "undocked"); }
    if (R_SUCCEEDED(mcuHwcInit())) { MCUHWC_GetBatteryLevel(&battery); mcuHwcExit(); }
}

/* Sleep cuts Wi-Fi, so the core denies every request; acks follow Rosalina's handler. */
static void on_sleep(u32 id) {
    if (R_FAILED(ptmSysmInit())) return;
    if (id == PTMNOTIFID_SLEEP_REQUESTED) {
        PTMSYSM_ReplyToSleepQuery(true);
        sleep_denied++;
        note("sleep denied");
    } else if (id == PTMNOTIFID_SLEEP_ALLOWED || id == PTMNOTIFID_GOING_TO_SLEEP ||
               id == PTMNOTIFID_FULLY_WAKING_UP || id == PTMNOTIFID_HALF_AWAKE) {
        PTMSYSM_NotifySleepPreparationComplete(ptmSysmGetNotificationAckValue(id));
        note("sleep notice %03lX acked", id);
    }
    ptmSysmExit();
}

/* ---- status ---- */

static void write_status(u64 up_s) {
    FILE *f = fopen(CORE_STATUS ".new", "w");
    if (!f) return;
    fprintf(f, "build %s\nuptime %llu\nlid %s\ndocked %s\n", RELAY_BUILD, up_s,
            lid_shut ? "shut" : "open", docked ? "yes" : "no");
    if (battery <= 100) fprintf(f, "battery %u\n", battery);
    fprintf(f, "sleep_denied %lu\n", sleep_denied);
    static const struct { MemRegion r; const char *name; } regions[] = {
        { MEMREGION_APPLICATION, "app" }, { MEMREGION_SYSTEM, "system" }, { MEMREGION_BASE, "base" } };
    for (int i = 0; i < 3; i++)
        fprintf(f, "mem_%s %lu %lu\n", regions[i].name,
                osGetMemRegionFree(regions[i].r), osGetMemRegionSize(regions[i].r));
    struct mallinfo mi = mallinfo();
    fprintf(f, "heap %u %u\n", mi.uordblks, HEAP_SIZE);
    FS_ArchiveResource res;
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD)))
        fprintf(f, "sd %llu %llu\n", (u64)res.freeClusters * res.clusterSize, (u64)res.totalClusters * res.clusterSize);
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_CTR_NAND)))
        fprintf(f, "nand %llu %llu\n", (u64)res.freeClusters * res.clusterSize, (u64)res.totalClusters * res.clusterSize);
    fclose(f);
    remove(CORE_STATUS);
    rename(CORE_STATUS ".new", CORE_STATUS);
}

/* ---- main ---- */

static bool safe_mode(void) {
    if (R_FAILED(hidInit())) return false;
    svcSleepThread(100 * 1000000LL);   /* first HID sample after init */
    hidScanInput();
    u32 held = hidKeysHeld();
    hidExit();
    return (held & (KEY_L | KEY_R)) == (KEY_L | KEY_R);
}

int main(void) {
    trim_log();
    if (safe_mode()) { note("safe mode: L+R held, core not started"); return 0; }
    note("start %s", RELAY_BUILD);

    FILE *run = fopen(CORE_RUN, "w");
    if (run) { fprintf(run, "%lu\n", core_boot_id()); fclose(run); }

    Handle notif;
    if (R_FAILED(srvEnableNotification(&notif))) { note("no notifications, exiting"); return 1; }
    static const u32 subs[] = { 0x100, PTMNOTIFID_SLEEP_REQUESTED, PTMNOTIFID_SLEEP_DENIED,
        PTMNOTIFID_SLEEP_ALLOWED, PTMNOTIFID_GOING_TO_SLEEP, PTMNOTIFID_FULLY_WAKING_UP,
        PTMNOTIFID_FULLY_AWAKE, PTMNOTIFID_HALF_AWAKE, PTMNOTIFID_SHUTDOWN,
        PTMNOTIFID_BATTERY_VERY_LOW, PTMNOTIFID_BATTERY_LOW, 0x213, 0x214 };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) srvSubscribe(subs[i]);

    u8 shell = 1;
    if (R_SUCCEEDED(ptmuInit())) { PTMU_GetShellState(&shell); ptmuExit(); }
    poll_power();
    set_lid(!shell);

    int ticks = 0;
    for (bool running = true; running;) {
        if (svcWaitSynchronization(notif, TICK_NS) == 0) {
            u32 id = 0;
            if (R_FAILED(srvReceiveNotification(&id))) continue;
            if (id == 0x100 || id == PTMNOTIFID_SHUTDOWN) { note("stop %03lX", id); running = false; }
            else if (id >= PTMNOTIFID_SLEEP_REQUESTED && id <= PTMNOTIFID_HALF_AWAKE) on_sleep(id);
            else if (id == 0x213) set_lid(false);   /* shell opened, also fired at system init */
            else if (id == 0x214) set_lid(true);    /* shell closed */
            else if (id == PTMNOTIFID_BATTERY_LOW || id == PTMNOTIFID_BATTERY_VERY_LOW) note("battery low %03lX", id);
            continue;
        }
        poll_power();
        if (lid_shut) lid_dark();
        if (++ticks % STATUS_TICKS == 0) write_status(svcGetSystemTick() / SYSCLOCK_ARM11);
    }

    if (lid_shut) lid_light();
    remove(CORE_RUN);
    svcCloseHandle(notif);
    return 0;
}
