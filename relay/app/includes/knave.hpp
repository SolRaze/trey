#pragma once

#include <stddef.h>

// Installs every CIA in knave's inbox (/srv/relay/inbox), moving each to inbox/done once installed.
// Blocks; msg gets the count for the screen. False when any failed (details in the FTP log).
bool installInbox(char* msg, size_t cap);

// Asks knave to start the Steam "Relay" target in steam-headless; its run.sh waits for the broadcast.
// Failure only logs: broadcasting works without it.
void wakeAzahar();
