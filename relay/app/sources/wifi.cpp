#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "ftp.h"
#include "wifi.hpp"

static bool acUp, nwmUp;

void wifiInit() {
    acUp = R_SUCCEEDED(acInit());
    nwmUp = R_SUCCEEDED(nwmExtInit());
}

void wifiExit() {
    if (nwmUp) nwmExtExit();
    if (acUp) acExit();
}

static const char* securityName(acSecurityMode m) {
    switch (m) {
        case AC_OPEN: return "Open";
        case AC_WEP_40BIT: case AC_WEP_104BIT: case AC_WEP_128BIT: return "WEP";
        case AC_WPA_TKIP: case AC_WPA_AES: return "WPA";
        case AC_WPA2_TKIP: case AC_WPA2_AES: return "WPA2";
    }
    return "";
}

void wifiPoll(Wifi& w) {
    w.strength = osGetWifiStrength();
    w.state = OS_SharedConfig->network_state;
    w.ap = 0;
    w.ssid[0] = 0;
    w.security = "";
    if (!acUp || R_FAILED(ACU_GetWifiStatus(&w.ap)) || !w.ap) { w.ap = 0; return; }
    char ssid[33] = { 0 };
    if (R_SUCCEEDED(ACU_GetSSID(ssid))) snprintf(w.ssid, sizeof(w.ssid), "%s", ssid);
    acSecurityMode m;
    if (R_SUCCEEDED(ACU_GetSecurityMode(&m))) w.security = securityName(m);
}

// System Settings' connection N is config block 0x80000+N, 0xC00 bytes. Offsets checked against slots
// saved by WifiManager: u16 version (0 = empty slot), u16 CRC-16/ARC of 0x4..0x413, enable, editable,
// then the network: SSID 0x20, SSID length, encryption (7 = WPA2 AES), password text 0x40,
// key 0x20 = PBKDF2-HMAC-SHA1(password, SSID, 4096). 0x340 DHCP, 0x341 auto DNS, 0x358..0x367 connection
// test result, IP, AP MAC and channel of the last connect, 0x412 MTU.
static const u32 SLOT_BLK = 0x80000, SLOT_SIZE = 0xC00;
static const int NET = 8, SSID_LEN = NET + 0x20, ENC = NET + 0x21, PASS = NET + 0x24, KEY = NET + 0x64;

static u16 crcArc(const u8* b, size_t n) {
    u16 c = 0;
    while (n--) { c ^= *b++; for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1; }
    return c;
}

static bool readSlot(int slot, u8* b) {
    return R_SUCCEEDED(CFG_GetConfigInfoBlk8(SLOT_SIZE, SLOT_BLK + slot, b));
}

void wifiReadSlots(Wifi& w) {
    w.slotsRead = false;
    memset(w.slot, 0, sizeof(w.slot));
    if (R_FAILED(cfguInit())) return;
    static u8 b[SLOT_SIZE];
    for (int i = 0; i < 3; i++) {
        if (!readSlot(i, b)) { ftp_log("wifi slot %d unreadable", i + 1); continue; }
        w.slotsRead = true;
        if (b[0] | b[1]) snprintf(w.slot[i], sizeof(w.slot[i]), "%.*s", b[SSID_LEN] > 32 ? 32 : b[SSID_LEN], (char*)b + NET);
        if ((b[0] | b[1]) && !w.slot[i][0]) snprintf(w.slot[i], sizeof(w.slot[i]), "saved");   // WPS setups keep no name here
    }
    cfguExit();
}

static bool ask(const char* hint, const char* initial, bool hidden, char* out, size_t cap) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, cap - 1);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, initial);
    if (hidden) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    return swkbdInputText(&kb, out, cap) == SWKBD_BUTTON_CONFIRM;
}

// b holds the slot as read; new slots get System Settings' defaults. False with msg set on failure.
static bool writeSlot(int slot, u8* b, const char* ssid, const char* pass, char* msg, size_t cap) {
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (!sl || sl > 32) { snprintf(msg, cap, "Network name needs 1 to 32 characters"); return false; }
    if (pl && (pl < 8 || pl > 63)) { snprintf(msg, cap, "Password needs 8 to 63 characters"); return false; }
    char oldSsid[33] = { 0 };
    if (b[0] | b[1]) memcpy(oldSsid, b + NET, b[SSID_LEN] > 32 ? 32 : b[SSID_LEN]);
    else {
        memset(b, 0, SLOT_SIZE);
        b[0] = 1;
        b[0x340] = b[0x341] = 1;
        b[0x412] = 1400 & 0xFF;
        b[0x413] = 1400 >> 8;
    }
    b[4] = b[5] = 1;
    memset(b + NET, 0, 0x84);
    memcpy(b + NET, ssid, sl);
    b[SSID_LEN] = sl;
    b[ENC] = pl ? 7 : 0;
    memcpy(b + PASS, pass, pl);
    if (pl) {
        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        int e = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
        if (!e) e = mbedtls_pkcs5_pbkdf2_hmac(&md, (const u8*)pass, pl, (const u8*)ssid, sl, 4096, 0x20, b + KEY);
        mbedtls_md_free(&md);
        if (e) { snprintf(msg, cap, "Key derivation failed"); return false; }
    }
    if (strcmp(ssid, oldSsid)) memset(b + 0x358, 0, 0x10);   // last connect's test, IP, AP, channel
    u16 c = crcArc(b + 4, 0x410);
    b[2] = c & 0xFF;
    b[3] = c >> 8;
    Result r = CFG_SetConfigInfoBlk8(SLOT_SIZE, SLOT_BLK + slot, b);
    if (R_SUCCEEDED(r)) r = CFG_UpdateConfigSavegame();
    if (R_FAILED(r)) { ftp_log("wifi slot %d write: %08lX", slot + 1, r); snprintf(msg, cap, "Could not save connection %d", slot + 1); return false; }
    ftp_log("wifi slot %d set: %s, %s", slot + 1, ssid, pl ? "WPA2" : "open");
    snprintf(msg, cap, "Saved, connecting");
    return true;
}

bool wifiSaveSlot(int slot, const char* ssid, const char* pass, char* msg, size_t cap) {
    if (slot < 0 || slot > 2) { snprintf(msg, cap, "No connection %d", slot + 1); return false; }
    if (R_FAILED(cfguInit())) { snprintf(msg, cap, "Settings unavailable"); return false; }
    static u8 b[SLOT_SIZE];
    bool ok = readSlot(slot, b);
    if (!ok) snprintf(msg, cap, "Connection %d unreadable", slot + 1);
    ok = ok && writeSlot(slot, b, ssid, pass, msg, cap);
    cfguExit();
    return ok;
}

bool wifiEditSlot(int slot, char* msg, size_t cap) {
    if (R_FAILED(cfguInit())) { snprintf(msg, cap, "Settings unavailable"); return false; }
    static u8 b[SLOT_SIZE];
    bool ok = false;
    char ssid[33] = { 0 }, pass[64] = { 0 }, oldSsid[33] = { 0 };
    if (!readSlot(slot, b)) snprintf(msg, cap, "Connection %d unreadable", slot + 1);
    else {
        if (b[0] | b[1]) {
            memcpy(oldSsid, b + NET, b[SSID_LEN] > 32 ? 32 : b[SSID_LEN]);
            memcpy(pass, b + PASS, 0x40);
            pass[63] = 0;
        }
        if (!ask("Network name", oldSsid, false, ssid, sizeof(ssid)) || !ssid[0]) snprintf(msg, cap, "Not changed");
        else {
            if (strcmp(ssid, oldSsid)) pass[0] = 0;
            if (!ask("Password, empty for an open network", pass, true, pass, sizeof(pass))) snprintf(msg, cap, "Not changed");
            else ok = writeSlot(slot, b, ssid, pass, msg, cap);
        }
    }
    memset(pass, 0, sizeof(pass));
    cfguExit();
    return ok;
}

bool wifiRadio(bool on, char* msg, size_t cap) {
    Result r = nwmUp ? NWMEXT_ControlWirelessEnabled(on) : -1;
    if (R_FAILED(r)) { ftp_log("wifi radio %d: %08lX", on, r); snprintf(msg, cap, "Could not turn Wi-Fi %s", on ? "on" : "off"); return false; }
    snprintf(msg, cap, on ? "Wi-Fi on, connecting" : "Wi-Fi off");
    return true;
}

bool wifiConnect(int slot, char* msg, size_t cap) {
    if (!acUp) { snprintf(msg, cap, "AC unavailable"); return false; }
    // An AC connect is refused while another one stands, so the radio cycles first.
    if (nwmUp) {
        NWMEXT_ControlWirelessEnabled(false);
        svcSleepThread(500000000LL);
        NWMEXT_ControlWirelessEnabled(true);
    }
    acuConfig cfg;
    Handle ev = 0;
    Result r = ACU_CreateDefaultConfig(&cfg);
    if (R_SUCCEEDED(r)) r = ACU_SetNetworkArea(&cfg, 2);
    if (R_SUCCEEDED(r)) r = ACU_SetAllowApType(&cfg, BIT(slot + 1));
    if (R_SUCCEEDED(r)) r = ACU_SetRequestEulaVersion(&cfg);
    if (R_SUCCEEDED(r)) r = svcCreateEvent(&ev, RESET_ONESHOT);
    if (R_SUCCEEDED(r)) r = ACU_ConnectAsync(&cfg, ev);
    if (R_SUCCEEDED(r)) r = svcWaitSynchronization(ev, 20000000000LL);
    if (ev) svcCloseHandle(ev);
    u32 ap = 0;
    ACU_GetWifiStatus(&ap);
    bool ok = R_SUCCEEDED(r) && (ap & BIT(slot + 1));
    if (!ok) {
        u32 code = 0;
        ACU_GetLastErrorCode(&code);
        ftp_log("wifi connect %d: %08lX ap %lu error %lu", slot + 1, r, ap, code);
        snprintf(msg, cap, "Connection %d failed", slot + 1);
    } else {
        ftp_log("wifi connected slot %d", slot + 1);
        snprintf(msg, cap, "Connected on connection %d", slot + 1);
    }
    return ok;
}

u64 settingsTitle() {
    u8 region = 0xFF;
    if (R_FAILED(cfguInit())) return 0;
    CFGU_SecureInfoGetRegion(&region);
    cfguExit();
    switch (region) {
        case CFG_REGION_JPN: return 0x0004001000020000ULL;
        case CFG_REGION_USA: return 0x0004001000021000ULL;
        case CFG_REGION_EUR: case CFG_REGION_AUS: return 0x0004001000022000ULL;
        case CFG_REGION_CHN: return 0x0004001000026000ULL;
        case CFG_REGION_KOR: return 0x0004001000027000ULL;
        case CFG_REGION_TWN: return 0x0004001000028000ULL;
    }
    return 0;
}
