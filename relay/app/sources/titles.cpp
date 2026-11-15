#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "ftp.h"
#include "saves.hpp"
#include "titles.hpp"

// relay itself, FBI-NH (the installer that can put relay back) and 3hs (CTR-N-3HSA).
static const u64 KEPT[] = { 0x000400000F0E7A00ULL, 0x000400000F800100ULL, 0x0004000003DF1000ULL };

static bool kept(u64 tid) {
    for (u64 k : KEPT) if (k == tid) return true;
    return false;
}

std::vector<AppTitle> appTitles() {
    std::vector<AppTitle> out;
    if (R_FAILED(amInit())) return out;
    static const FS_MediaType media[] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
    for (FS_MediaType m : media) {
        u32 count = 0, got = 0;
        if (R_FAILED(AM_GetTitleCount(m, &count)) || !count) continue;
        std::vector<u64> ids(count);
        if (R_FAILED(AM_GetTitleList(&got, m, count, ids.data()))) continue;
        for (u32 i = 0; i < got; i++) {
            if ((ids[i] >> 32) != 0x00040000) continue;   // applications only
            AppTitle t{ ids[i], (u8)m, "", 0, false };
            titleName(t.tid, m, t.name, sizeof(t.name));
            AM_TitleInfo info;
            if (R_SUCCEEDED(AM_GetTitleInfo(m, 1, &t.tid, &info))) t.size = info.size;
            t.kept = m != MEDIATYPE_SD || kept(t.tid);
            out.push_back(t);
        }
    }
    amExit();
    std::sort(out.begin(), out.end(), [](const AppTitle& a, const AppTitle& b) { return strcasecmp(a.name, b.name) < 0; });
    return out;
}

bool uninstallTitle(u64 tid, char* msg, size_t cap) {
    for (auto& t : appTitles()) {
        if (t.tid != tid) continue;
        if (t.kept) { snprintf(msg, cap, "%s is kept", t.name); return false; }
        // AM deletes the save with the title.
        if (!saveStored(tid, t.media)) { snprintf(msg, cap, "%.40s: save not stored, see log; not uninstalled", t.name); return false; }
        if (R_FAILED(amInit())) { snprintf(msg, cap, "AM unavailable"); return false; }
        Result r = AM_DeleteTitle(MEDIATYPE_SD, tid);
        // Its update and DLC titles; absent is fine.
        if (R_SUCCEEDED(r)) for (u32 hi : { 0x0004000Eu, 0x0004008Cu }) AM_DeleteTitle(MEDIATYPE_SD, (u64)hi << 32 | (u32)tid);
        amExit();
        ftp_log("uninstall %016llX %s: %08lX", tid, t.name, r);
        if (R_FAILED(r)) { snprintf(msg, cap, "Uninstall failed %08lX", r); return false; }
        forgetSync(tid);   // a reinstall starts a new save; without this its first sync replaces the stored one unkept
        snprintf(msg, cap, "%s uninstalled", t.name);
        return true;
    }
    snprintf(msg, cap, "%016llX not installed", tid);
    return false;
}
