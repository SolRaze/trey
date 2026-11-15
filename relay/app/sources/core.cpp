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

static bool installed() {
    FILE* f = fopen(CORE_CXI, "rb");
    if (!f) return false;
    bool same = true;
    static u8 buf[4096];
    size_t off = 0, n;
    while (same && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        same = off + n <= core_bin_size && !memcmp(buf, core_bin + off, n);
        off += n;
    }
    fclose(f);
    return same && off == core_bin_size;
}

static bool install() {
    if (installed()) return true;
    mkdir("/luma/sysmodules", 0777);
    FILE* f = fopen(CORE_CXI ".new", "wb");
    if (!f) return false;
    bool ok = fwrite(core_bin, 1, core_bin_size, f) == core_bin_size;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(CORE_CXI); ok = rename(CORE_CXI ".new", CORE_CXI) == 0; }
    if (!ok) remove(CORE_CXI ".new");
    return ok;
}

// Process names come from Luma's svcGetProcessInfo extension (type 0x10000).
static bool running() {
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64))) return false;
    for (s32 i = 0; i < n; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) continue;
        char name[9] = {};
        svcGetProcessInfo((s64*)name, h, 0x10000);
        svcCloseHandle(h);
        if (!strncmp(name, CORE_NAME, 8)) return true;
    }
    return false;
}

void coreStart() {
    hidScanInput();
    if ((hidKeysHeld() & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) { ftp_log("core skipped: L+R held"); return; }
    if (!install()) { ftp_log("core: cannot write " CORE_CXI); return; }
    if (running()) { ftp_log("core running"); return; }

    u32 boot = core_boot_id(), last = 0, launches = 0;
    if (FILE* f = fopen(CORE_LAUNCHES, "r")) {
        if (fscanf(f, "%lu %lu", &last, &launches) != 2) launches = 0;
        fclose(f);
    }
    if (last != boot) launches = 0;
    if (launches >= 2) { ftp_log("core off: stopped twice this boot, see core.log"); return; }
    if (FILE* f = fopen(CORE_LAUNCHES, "w")) { fprintf(f, "%lu %lu\n", boot, launches + 1); fclose(f); }

    if (R_FAILED(pmAppInit())) { ftp_log("core: pm:app unavailable"); return; }
    FS_ProgramInfo info = {};
    info.programId = CORE_TID;
    info.mediaType = MEDIATYPE_NAND;
    Result r = PMAPP_LaunchTitle(&info, 0);
    pmAppExit();
    if (R_FAILED(r)) ftp_log("core launch failed %08lX", r);
    else ftp_log(launches ? "core restarted after a stop" : "core started");
}
