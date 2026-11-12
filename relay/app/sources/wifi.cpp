#include <3ds.h>
#include <stdio.h>
#include <string.h>

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

void wifiReadSlots(Wifi& w) {
    w.slotsRead = false;
    memset(w.slot, 0, sizeof(w.slot));
    if (!acUp) return;
    for (u32 i = 0; i < 3; i++) {
        char ssid[33] = { 0 };
        Result r = ACI_LoadNetworkSetting(i);
        if (R_SUCCEEDED(r)) r = ACI_GetNetworkWirelessEssidSecuritySsid(ssid);
        if (R_FAILED(r)) { ftp_log("wifi slot %lu: %08lX", i + 1, r); continue; }
        w.slotsRead = true;
        snprintf(w.slot[i], sizeof(w.slot[i]), "%.32s", ssid);
    }
    ftp_log("wifi state %u ap %lu strength %u", w.state, w.ap, w.strength);
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
