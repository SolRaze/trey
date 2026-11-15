#pragma once

#include <3ds.h>
#include <stddef.h>

#include <vector>

struct AppTitle {
    u64 tid;
    u8 media;
    char name[64];     // SMDH short name, the title id when the icon is unreadable
    u64 size;          // bytes on the medium
    bool kept;         // relay, FBI and 3hs never uninstall; card titles cannot
};

// Installed applications on SD and card, by name.
std::vector<AppTitle> appTitles();
// Uninstalls an SD application with its update and DLC through AM. Its save is synced to relay's
// store first; refused when that fails, and for kept titles. msg says how it went.
bool uninstallTitle(u64 tid, char* msg, size_t cap);
