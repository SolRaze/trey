#pragma once

#include <stddef.h>

// True while the console clock reads before 2024, as after the RTC lost power.
bool clockWrong();
// Sets the clock from NTP, the way Rosalina's "Set time via NTP" does. Blocks up to ~3 s; needs Wi-Fi.
bool clockSync(char* msg, size_t cap);
