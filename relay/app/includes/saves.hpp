#pragma once

#include <3ds.h>
#include <stddef.h>
#include <time.h>

#include <vector>

// Syncs every installed application's save and extdata with its copy in sdmc:/3ds/relay/saves, both ways.
// Blocks; msg gets the counts for the screen. False when any title failed (details in the FTP log).
bool syncSaves(char* msg, size_t cap);

// Syncs the title's save now. False when the store does not hold it afterwards: unreadable, over the
// size limit, or the card write failed (details in the FTP log). True when there is no save.
bool saveStored(u64 tid, u8 media);

// Drops the title's last-synced hashes: its next sync then keeps the store's copy as a backup before
// replacing it. For a title whose console save is gone, as after an uninstall.
void forgetSync(u64 tid);

// SMDH short name in English, Japanese when that is empty; the title id when the icon is unreadable.
void titleName(u64 tid, u8 media, char* out, size_t cap);

struct SaveTitle {
    u64 tid;
    u8 media;
    char name[64];     // SMDH short name, the title id when the icon is unreadable
    time_t synced;     // last sync of its save, 0 never
    bool ext;          // has an extdata copy
};

struct SaveBackup {
    char label[48];    // "13 Nov 2026 04:30  save"
    char path[192];    // relay pack file
    bool ext;
};

// Titles with a copy in the store, by name. Needs no AM session of the caller.
std::vector<SaveTitle> saveTitles();
// Relay backups of the title, newest first.
std::vector<SaveBackup> saveBackups(const SaveTitle& t);
// Puts the backup over the console's save or extdata; what it replaces becomes a backup first.
bool restoreBackup(const SaveTitle& t, const SaveBackup& b, char* msg, size_t cap);
