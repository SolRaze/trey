#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core.h"
#include "core.hpp"
#include "core_bin.h"
#include "ftp.h"
#include "ts_node.h"
#include "tsmod_bin.h"

// One background sysmodule Relay owns: where its .cxi goes, its title and process name, its crash guard,
// and the file that makes it exit on its own (null: the module has none, a new build waits for next boot).
struct Module {
    const char* label;
    const char* cxi;
    u64 tid;
    const char* proc;
    const char* launches;
    const char* stop;
    const u8* bin;
    size_t size;
};

static const Module coreMod = { "core", CORE_CXI, CORE_TID, CORE_NAME, CORE_LAUNCHES, nullptr, core_bin, core_bin_size };
static const Module tsMod = { "ts", TS_CXI, TS_TID, TS_NAME, TS_LAUNCHES, TS_STOP, tsmod_bin, tsmod_bin_size };

static bool installed(const Module& m) {
    FILE* f = fopen(m.cxi, "rb");
    if (!f) return false;
    bool same = true;
    static u8 buf[4096];
    size_t off = 0, n;
    while (same && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        same = off + n <= m.size && !memcmp(buf, m.bin + off, n);
        off += n;
    }
    fclose(f);
    return same && off == m.size;
}

static bool install(const Module& m, bool* changed) {
    *changed = false;
    if (installed(m)) return true;
    *changed = true;
    mkdir("/luma/sysmodules", 0777);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.new", m.cxi);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(m.bin, 1, m.size, f) == m.size;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(m.cxi); ok = rename(tmp, m.cxi) == 0; }
    if (!ok) remove(tmp);
    return ok;
}

static void start(const Module& m) {
    hidScanInput();
    if ((hidKeysHeld() & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) { ftp_log("%s skipped: L+R held", m.label); return; }
    bool changed, fresh = false;
    if (!install(m, &changed)) { ftp_log("%s: cannot write %s", m.label, m.cxi); return; }
    // A running module is never terminated: PMAPP_TerminateTitle then a relaunch faults the kernel
    // in loader (Luma 13.5). One with a stop file exits on its own and relaunches fine.
    if (proc_pid(m.proc, nullptr)) {
        if (!changed || !m.stop) { ftp_log("%s running", m.label); return; }
        if (FILE* f = fopen(m.stop, "w")) fclose(f);
        for (int i = 0; i < 50 && proc_pid(m.proc, nullptr); i++) svcSleepThread(100 * 1000000LL);
        if (proc_pid(m.proc, nullptr)) { remove(m.stop); ftp_log("%s running, new build next boot", m.label); return; }
        ftp_log("%s stopped for a new build", m.label);
        fresh = true;
    }
    if (m.stop) remove(m.stop);   // a stale one would stop the new process at its first tick

    u32 boot = core_boot_id(), last = 0, launches = 0;
    if (FILE* f = fopen(m.launches, "r")) {
        if (fscanf(f, "%lu %lu", &last, &launches) != 2) launches = 0;
        fclose(f);
    }
    if (last != boot || fresh) launches = 0;   // a deliberate stop is not a crash
    if (launches >= 2) { ftp_log("%s off: stopped twice this boot, see its log", m.label); return; }
    if (FILE* f = fopen(m.launches, "w")) { fprintf(f, "%lu %lu\n", boot, launches + 1); fclose(f); }

    if (R_FAILED(pmAppInit())) { ftp_log("%s: pm:app unavailable", m.label); return; }
    FS_ProgramInfo info = {};
    info.programId = m.tid;
    info.mediaType = MEDIATYPE_NAND;
    Result r = PMAPP_LaunchTitle(&info, 0);
    pmAppExit();
    if (R_FAILED(r)) ftp_log("%s launch failed %08lX", m.label, r);
    else ftp_log(launches ? "%s restarted after a stop" : "%s started", m.label);
}

void coreStart() { start(coreMod); }

void tsStart() {
    mkdir(TS_DIR, 0777);
    struct stat st;
    if (stat(TS_KEY, &st) != 0) { ftp_log("ts off: no key file"); return; }
    start(tsMod);
}

static bool readStatus(char* out, size_t cap) {
    snprintf(out, cap, "off");
    if (!proc_pid(TS_NAME, nullptr)) return false;
    FILE* f = fopen(TS_STATUS, "r");
    if (!f) { snprintf(out, cap, "starting"); return false; }
    char line[64] = "";
    bool ok = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    line[strcspn(line, "\n")] = 0;
    if (!ok || line[0] < '0' || line[0] > '1' || line[1] != ' ') { snprintf(out, cap, "starting"); return false; }
    snprintf(out, cap, "%s", line + 2);
    return line[0] == '1';
}

// The module's status file; site.c and the tunnel row read the node through this. The row asks every
// frame, so a read (process walk + SD file) happens once a second at most. Called from the main thread
// and the FTP thread: a torn copy shows one wrong frame at worst.
extern "C" bool ts_node_line(char* out, size_t cap) {
    static char line[48];
    static bool up;
    static u64 at;
    u64 now = osGetTime();
    if (!at || now - at >= 1000) { up = readStatus(line, sizeof(line)); at = now; }
    snprintf(out, cap, "%s", line);
    return up;
}
