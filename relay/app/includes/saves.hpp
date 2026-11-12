#pragma once

#include <stddef.h>

// Syncs every installed application's save with Azahar's copy on knave, both ways. Blocks; msg gets
// the counts for the screen. False when any title failed (details in the FTP log).
bool syncSaves(char* msg, size_t cap);
