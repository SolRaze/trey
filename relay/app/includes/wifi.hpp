#pragma once

#include <3ds.h>
#include <stddef.h>

// Wi-Fi state for the screen and the Wi-Fi page's actions. AC slots are System Settings'
// connection 1 to 3, indexed 0 to 2 here.
struct Wifi {
    u32 ap;              // AC_AP_TYPE_SLOT* bit of the connected slot, 0 when not connected
    u8 strength;         // 0 to 3, HOME Menu's bars
    u8 state;            // shared page network_state, raw; logged on page open
    char ssid[33];       // connected network, "" when none
    const char* security;
    char slot[3][33];    // saved slot names, "" when empty; slotsRead false when ac:i refused
    bool slotsRead;
};

void wifiInit();
void wifiExit();

// Cheap: AC status, SSID, bars. Called once a second.
void wifiPoll(Wifi& w);

// Reads the three saved slot names over ac:i. On page open only.
void wifiReadSlots(Wifi& w);

// Wireless off, on, then AC connect to that slot only. Blocks up to 20 s; msg says how it went.
bool wifiConnect(int slot, char* msg, size_t cap);

// Wireless radio on or off, as the HOME Menu toggle does. FTP drops while off.
bool wifiRadio(bool on, char* msg, size_t cap);

// System Settings for this region, for changing or adding networks; 0 when the region is unknown.
u64 settingsTitle();
