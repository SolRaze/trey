#pragma once

#include <stddef.h>

// Installs every CIA in sdmc:/3ds/relay/inbox, moving each to inbox/done once installed. Files land
// there over FTP or straight on the card. Blocks; msg gets the count for the screen. False when any
// failed (details in the FTP log).
bool installInbox(char* msg, size_t cap);
