#pragma once

#include <stddef.h>

// Syncs every installed application's save with its copy in sdmc:/3ds/relay/saves, both ways. Blocks;
// msg gets the counts for the screen. False when any title failed (details in the FTP log).
bool syncSaves(char* msg, size_t cap);
