#pragma once

#include <stddef.h>

// The latest GitHub release of SolRaze/trey carries `build` (the id) and `relay.cia`; `make release`
// uploads both. Public repo, so no token on the console.
#define UPDATE_BASE "https://github.com/SolRaze/trey/releases/latest/download/"

enum class UpdateResult { Current, Newer, Installed, Failed };

// Reads the latest release's build id. Current when it is not newer than this build, Newer when it
// is; msg gets a line for the screen either way. Nothing is installed.
UpdateResult checkUpdate(char* msg, size_t cap);

// Streams a CIA from url into the SD installer; never commits a partial one. msg says why on false.
bool installCia(const char* url, char* msg, size_t cap);

// Same, from a file on the card.
bool installCiaFile(const char* path, char* msg, size_t cap);

// checkUpdate, then on Newer streams relay.cia into the installer. Blocks for the download.
UpdateResult runUpdate(char* msg, size_t cap);
