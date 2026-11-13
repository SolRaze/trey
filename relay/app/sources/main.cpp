#include <3ds.h>
#include <citro2d.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "arm.hpp"
#include "clock.hpp"
#include "inbox.hpp"
#include "net.hpp"
#include "saves.hpp"
#include "update.hpp"
#include "wifi.hpp"
#include "ftp.h"
#include "site.h"
#include "led.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

// Themes. mono: matte near-black field, white
// type, one pale accent for live, red only for faults. memcard: case's lit grey field, light grey
// type with a black border, yellow titles, PS2 browser menu: selected row sky blue, the rest grey, both
// black-bordered (blue sampled from ~/Reference/screens/ps2/delete.mp4).
struct Theme {
    const char* name;
    u32 field[4];         // top screen corner colours: top left, top right, bottom left, bottom right
    u32 bottom[4];        // bottom screen corners, darker in memcard so the dark grey rows sit closer to it
    u32 white, grey, dim, live, fault;
    u32 sel, idle;        // menu rows: selected title, other titles
    bool outline;         // border round every glyph, case's ink.ts look
};
static const Theme THEMES[] = {
    { "mono", { C2D_Color32(12, 12, 14, 255), C2D_Color32(12, 12, 14, 255),
                C2D_Color32(12, 12, 14, 255), C2D_Color32(12, 12, 14, 255) },
              { C2D_Color32(12, 12, 14, 255), C2D_Color32(12, 12, 14, 255),
                C2D_Color32(12, 12, 14, 255), C2D_Color32(12, 12, 14, 255) },
      C2D_Color32(236, 236, 238, 255), C2D_Color32(150, 150, 156, 255), C2D_Color32(122, 122, 128, 255),
      C2D_Color32(158, 214, 190, 255), C2D_Color32(230, 104, 104, 255),
      C2D_Color32(236, 236, 238, 255), C2D_Color32(150, 150, 156, 255), false },
    { "memcard", { C2D_Color32(196, 196, 196, 255), C2D_Color32(110, 110, 110, 255),
                   C2D_Color32(120, 120, 120, 255), C2D_Color32(54, 54, 54, 255) },
                 { C2D_Color32(132, 132, 132, 255), C2D_Color32(84, 84, 84, 255),
                   C2D_Color32(90, 90, 90, 255), C2D_Color32(40, 40, 40, 255) },
      C2D_Color32(243, 228, 122, 255), C2D_Color32(214, 214, 214, 255), C2D_Color32(196, 196, 196, 255),
      C2D_Color32(243, 228, 122, 255), C2D_Color32(255, 120, 120, 255),
      C2D_Color32(118, 206, 248, 255), C2D_Color32(70, 72, 72, 255), true },
};
static const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static const char* THEME_FILE = "sdmc:/3ds/relay/theme";
static const Theme* T = &THEMES[0];

static int loadTheme() {
    char name[32] = "";
    FILE* f = fopen(THEME_FILE, "r");
    if (f) { if (!fgets(name, sizeof(name), f)) name[0] = '\0'; fclose(f); }
    name[strcspn(name, "\r\n")] = '\0';
    for (int i = 0; i < THEME_COUNT; i++) if (!strcmp(name, THEMES[i].name)) return i;
    return 0;
}

static void saveTheme(int i) {
    FILE* f = fopen(THEME_FILE, "w");
    if (f) { fputs(THEMES[i].name, f); fclose(f); }
}

static const int TOP_W = 400, BOT_W = 320;
static const int MARGIN = 24;

// Bottom screen menu: one line per row, the selected row's description or result below them.
// Touch hits the whole row. Pages share the layout: main; Wi-Fi and Saves under it, Backups under
// Saves (B back). At most VIS rows show; longer lists scroll under their first PINNED rows.
static const int ROW_TOP = 8, ROW_H = 25, GROUP_GAP = 6, DESC_Y = 182, VIS = 6, MAX_ROWS = 128;
enum { ITEM_BROADCAST, ITEM_SAVES, ITEM_INBOX, ITEM_WIFI, ITEM_UPDATE, ITEM_THEME, ITEM_COUNT };
static const char* ITEM_TITLE[] = { "Broadcast a game", "Saves", "Install from inbox", "Wi-Fi", "Update", "Theme" };
enum { WIFI_SLOT1, WIFI_SLOT2, WIFI_SLOT3, WIFI_RADIO, WIFI_SETTINGS, WIFI_CLOCK, WIFI_COUNT };
// Bit i set = a group starts at visible row i. Main: Broadcast | Saves, Inbox | Wi-Fi, Update, Theme.
// Wi-Fi: the three connections | radio, settings, clock. Saves: Sync now | titles.
static const u32 MAIN_GROUPS = BIT(ITEM_SAVES) | BIT(ITEM_WIFI), WIFI_GROUPS = BIT(WIFI_RADIO), SAVES_GROUPS = BIT(1);
static u32 groups = MAIN_GROUPS;
static int rowY(int i) {
    int y = ROW_TOP + i * ROW_H;
    for (int k = 1; k <= i; k++) if (groups & BIT(k)) y += GROUP_GAP;
    return y;
}

struct Row { const char* title; const char* desc; bool enabled; bool mark; };

static C2D_TextBuf textBuf;
static C2D_TextBuf measureBuf;  // cleared per measurement so long paths never fill textBuf

static void text(float x, float y, float scale, u32 color, const char* str, u32 flags = 0) {
    C2D_Text t;
    C2D_TextParse(&t, textBuf, str);
    C2D_TextOptimize(&t);
    if (T->outline) {
        // Sub-pixel offsets: glyphs sample linearly, so this is a thin border that leaves small text's
        // counters open; whole-pixel offsets fill them at 240 lines.
        static const float OFF[8][2] = { {-.6f,-.6f}, {0,-.8f}, {.6f,-.6f}, {-.8f,0}, {.8f,0}, {-.6f,.6f}, {0,.8f}, {.6f,.6f} };
        for (auto& o : OFF)
            C2D_DrawText(&t, C2D_WithColor | flags, x + o[0], y + o[1], 0.5f, scale, scale, C2D_Color32(0, 0, 0, 255));
    }
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void field(const u32* c, int w) {
    C2D_DrawRectangle(0, 0, 0, w, 240, c[0], c[1], c[2], c[3]);
}

static float width(const char* str, float scale) {
    C2D_Text t;
    C2D_TextBufClear(measureBuf);
    C2D_TextParse(&t, measureBuf, str);
    float w = 0;
    C2D_TextGetDimensions(&t, scale, scale, &w, nullptr);
    return w;
}

// Fits str into max_w at scale by cutting from the front, keeping the file name end.
static void fitTail(char* out, size_t cap, const char* str, float scale, float max_w) {
    snprintf(out, cap, "%s", str);
    for (const char* p = str; width(out, scale) > max_w && *p; ) {
        do p++; while ((*p & 0xC0) == 0x80);   // whole UTF-8 characters
        snprintf(out, cap, "...%s", p);
    }
}

// Same from the back, for names.
static void fitHead(char* out, size_t cap, const char* str, float scale, float max_w) {
    snprintf(out, cap, "%s", str);
    size_t n = strlen(out);
    while (n && width(out, scale) > max_w) {
        do n--; while (n && (str[n] & 0xC0) == 0x80);
        snprintf(out, cap, "%.*s...", (int)n, str);
    }
}

static void formatBytes(char* out, size_t cap, unsigned long long b) {
    if (b >= 1000ull * 1000 * 1000) snprintf(out, cap, "%.2f GB", b / 1e9);
    else if (b >= 1000ull * 1000) snprintf(out, cap, "%.1f MB", b / 1e6);
    else if (b >= 1000) snprintf(out, cap, "%.0f KB", b / 1e3);
    else snprintf(out, cap, "%llu B", b);
}

// Selection light: a soft band brightest at the centre, faded to nothing at both ends.
static void streak(int y, int h, float alpha) {
    if (alpha <= 0.01f) return;
    u8 a = (u8)(alpha * 34);
    u32 clear = C2D_Color32(255, 255, 255, 0), lit = C2D_Color32(255, 255, 255, a);
    C2D_DrawRectangle(0, y, 0, BOT_W / 2, h, clear, lit, clear, lit);
    C2D_DrawRectangle(BOT_W / 2, y, 0, BOT_W / 2, h, lit, clear, lit, clear);
    u32 edge = C2D_Color32(255, 255, 255, (u8)(alpha * 60));
    u32 edgeClear = C2D_Color32(255, 255, 255, 0);
    C2D_DrawRectangle(0, y, 0, BOT_W / 2, 1, edgeClear, edge, edgeClear, edge);
    C2D_DrawRectangle(BOT_W / 2, y, 0, BOT_W / 2, 1, edge, edgeClear, edge, edgeClear);
    C2D_DrawRectangle(0, y + h - 1, 0, BOT_W / 2, 1, edgeClear, edge, edgeClear, edge);
    C2D_DrawRectangle(BOT_W / 2, y + h - 1, 0, BOT_W / 2, 1, edge, edgeClear, edge, edgeClear);
}

// HOME Menu style bars, rising left to right; lit count = strength 0 to 3.
static void bars(float x, float y, int strength, u32 on, u32 off) {
    for (int i = 0; i < 3; i++) {
        int h = 4 + i * 3;
        C2D_DrawRectSolid(x + i * 6, y + 10 - h, 0, 4, h, i < strength ? on : off);
    }
}

enum class State { Starting, Ready, NoWifi, NoSockets, Armed };

struct Screen {
    State state = State::Starting;
    u32 ip = 0;
    const char* fault = nullptr;  // arm failure, shown until the next attempt
    ftp_status_t ftp = {};
    Wifi wifi = {};
    int battery = -1;            // percent, -1 unknown
    bool charging = false;
    double sdFreeGB = -1;
    unsigned long long rate = 0; // current transfer, bytes per second
};

// Battery and card space for the header, every few seconds.
static void pollPower(Screen& s) {
    u8 level = 0, charge = 0;
    if (R_SUCCEEDED(mcuHwcInit())) {
        s.battery = R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level)) ? level : -1;
        mcuHwcExit();
    }
    if (R_SUCCEEDED(ptmuInit())) { PTMU_GetBatteryChargeState(&charge); ptmuExit(); s.charging = charge; }
    FS_ArchiveResource res;
    if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&res)))
        s.sdFreeGB = (double)res.freeClusters * res.clusterSize / 1e9;
}

static void header(const Screen& s, const char* title) {
    char line[64];
    text(MARGIN, 16, 0.55f, T->grey, title);
    int n = 0;
    line[0] = 0;
    if (s.battery >= 0) n += snprintf(line, sizeof(line), "%d%%%s", s.battery, s.charging ? " charging" : "");
    if (s.sdFreeGB >= 0) snprintf(line + n, sizeof(line) - n, "%sSD %.1f GB free", n ? "    " : "", s.sdFreeGB);
    u32 c = s.battery >= 0 && s.battery <= 15 && !s.charging ? T->fault : T->dim;
    text(TOP_W - MARGIN, 16, 0.5f, c, line, C2D_AlignRight);
}

static void drawTop(const Screen& s) {
    char line[160];
    header(s, "Relay");

    u32 mark = T->dim;
    const char* label = "Starting";
    switch (s.state) {
        case State::Ready:     mark = T->live;  label = "FTP ready"; break;
        case State::NoWifi:    mark = T->fault; label = "No Wi-Fi"; break;
        case State::NoSockets: mark = T->fault; label = "Network failed to start"; break;
        case State::Armed:     mark = T->live;  label = "Broadcast armed"; break;
        case State::Starting:  break;
    }
    C2D_DrawRectSolid(MARGIN, 76, 0, 8, 8, mark);
    text(MARGIN + 16, 70, 0.55f, s.state == State::Ready || s.state == State::Armed ? T->white : T->grey, label);
    if (s.wifi.ap) text(TOP_W - MARGIN, 70, 0.5f, T->grey, s.wifi.ssid, C2D_AlignRight);

    if (s.state == State::Armed) {
        text(MARGIN, 100, 0.95f, T->white, "Pick a game on HOME");
        text(MARGIN, 152, 0.5f, T->grey, "FTP returns inside the game");
        return;
    }

    if (s.ip) {
        struct in_addr a = { .s_addr = s.ip };
        snprintf(line, sizeof(line), "%s:%d", inet_ntoa(a), FTP_PORT);
        text(MARGIN, 100, 1.1f, T->white, line);
    } else {
        text(MARGIN, 100, 1.1f, T->dim, "-");
    }

    if (s.fault) {
        text(MARGIN, 152, 0.55f, T->fault, s.fault);
        return;
    }
    if (s.ftp.sessions > 0) snprintf(line, sizeof(line), "%d FTP client%s", s.ftp.sessions, s.ftp.sessions > 1 ? "s" : "");
    else snprintf(line, sizeof(line), "Idle");
    text(MARGIN, 152, 0.55f, T->grey, line);

    if (s.ftp.last[0]) {
        fitTail(line, sizeof(line), s.ftp.last, 0.5f, TOP_W - 2 * MARGIN);
        text(MARGIN, 176, 0.5f, T->dim, line);
    }
    if (s.ftp.xfer) {
        // Gets know their size: bar + percent. Puts arrive with no size: bytes and speed only.
        char done[24], size[24], rate[24];
        formatBytes(done, sizeof(done), s.ftp.xfer_done);
        formatBytes(rate, sizeof(rate), s.rate);
        float w = TOP_W - 2 * MARGIN;
        if (s.ftp.xfer_size) {
            float f = (float)s.ftp.xfer_done / s.ftp.xfer_size;
            f = f > 1 ? 1 : f;
            C2D_DrawRectSolid(MARGIN, 200, 0, w, 3, T->dim);
            C2D_DrawRectSolid(MARGIN, 200, 0, w * f, 3, T->live);
            formatBytes(size, sizeof(size), s.ftp.xfer_size);
            snprintf(line, sizeof(line), "%d%%    %s of %s    %s/s", (int)(f * 100), done, size, rate);
        } else {
            snprintf(line, sizeof(line), "%s %s    %s/s", s.ftp.xfer == 2 ? "received" : "sent", done, rate);
        }
        text(MARGIN, 208, 0.45f, T->grey, line);
    } else if (s.ftp.rx || s.ftp.tx) {
        char rx[24], tx[24];
        formatBytes(rx, sizeof(rx), s.ftp.rx);
        formatBytes(tx, sizeof(tx), s.ftp.tx);
        snprintf(line, sizeof(line), "received %s    sent %s", rx, tx);
        text(MARGIN, 208, 0.45f, T->dim, line);
    }
}

static void drawWifiTop(const Screen& s) {
    char line[96];
    header(s, "Wi-Fi");
    const Wifi& w = s.wifi;
    if (!w.ap) {
        text(MARGIN, 70, 0.55f, T->grey, "Not connected");
        text(MARGIN, 100, 1.1f, T->dim, "-");
        text(MARGIN, 152, 0.5f, T->grey, "Pick a connection below");
        return;
    }
    bars(MARGIN, 74, w.strength, T->live, T->dim);
    static const char* QUALITY[] = { "No signal", "Weak signal", "Fair signal", "Good signal" };
    text(MARGIN + 24, 70, 0.55f, T->white, QUALITY[w.strength > 3 ? 3 : w.strength]);
    text(MARGIN, 100, 1.1f, T->white, w.ssid[0] ? w.ssid : "Connected");
    if (s.ip) {
        struct in_addr a = { .s_addr = s.ip };
        snprintf(line, sizeof(line), "%s:%d", inet_ntoa(a), FTP_PORT);
        text(MARGIN, 152, 0.55f, T->grey, line);
    }
    int slot = w.ap & AC_AP_TYPE_SLOT1 ? 1 : w.ap & AC_AP_TYPE_SLOT2 ? 2 : w.ap & AC_AP_TYPE_SLOT3 ? 3 : 0;
    if (slot) snprintf(line, sizeof(line), "Connection %d    %s", slot, w.security);
    else snprintf(line, sizeof(line), "%s", w.security);
    text(MARGIN, 176, 0.5f, T->dim, line);
}

static void formatWhen(char* out, size_t cap, time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);   // the 3DS clock is local time with no zone
    strftime(out, cap, "%d %b %Y  %H:%M", &tm);
}

// Saves and Backups pages: t null = the Sync now row.
static void drawSavesTop(const Screen& s, const SaveTitle* t, int titles, const SaveBackup* b, int backups) {
    char line[96], name[96];
    header(s, b ? "Backups" : "Saves");
    if (!t) {
        text(MARGIN, 70, 0.55f, T->grey, "Saves and extdata, both ways");
        snprintf(line, sizeof(line), "%d title%s", titles, titles == 1 ? "" : "s");
        text(MARGIN, 100, 1.1f, T->white, line);
        text(MARGIN, 152, 0.55f, T->grey, "/3ds/relay/saves");
        text(MARGIN, 176, 0.5f, T->dim, "Also runs at launch and before Broadcast");
        return;
    }
    snprintf(line, sizeof(line), "%016llX    %s", t->tid, t->media == MEDIATYPE_GAME_CARD ? "Game card" : "SD card");
    text(MARGIN, 70, 0.5f, T->grey, line);
    fitHead(name, sizeof(name), t->name, 0.95f, TOP_W - 2 * MARGIN);
    text(MARGIN, 100, 0.95f, T->white, name);
    if (b) {
        text(MARGIN, 152, 0.55f, T->grey, b->label);
        snprintf(line, sizeof(line), "%d backup%s", backups, backups == 1 ? "" : "s");
        text(MARGIN, 176, 0.5f, T->dim, line);
        return;
    }
    if (t->synced) {
        char when[32];
        formatWhen(when, sizeof(when), t->synced);
        snprintf(line, sizeof(line), "Copy from %s", when);
    } else {
        snprintf(line, sizeof(line), "Checkpoint backups only");
    }
    text(MARGIN, 152, 0.55f, T->grey, line);
    text(MARGIN, 176, 0.5f, T->dim, t->ext ? "Save and extdata" : "Save");
}

// vis maps each visible slot to its row; light is per slot.
static void drawBottom(const Row* rows, int count, int pinned, const int* vis, int nvis, int selected,
                       const float* light, bool locked, const char* prompt) {
    char line[128];
    for (int v = 0; v < nvis; v++) {
        const Row& r = rows[vis[v]];
        if (!T->outline) streak(rowY(v), ROW_H, light[v]);  // PS2 browser marks selection by colour only
        bool on = vis[v] == selected && !locked;
        u32 c = on ? T->sel : r.enabled ? T->idle : T->dim;
        fitHead(line, sizeof(line), r.title, 0.58f, BOT_W - 2 * MARGIN - (r.mark ? 16 : 0));
        text(MARGIN, rowY(v) + 5, 0.58f, c, line);
        if (r.mark) C2D_DrawRectSolid(BOT_W - MARGIN - 8, rowY(v) + 10, 0, 8, 8, T->live);
    }
    // Scroll position: a thin bar on the right edge beside the scrolling part.
    int list = count - pinned, shown = nvis - pinned;
    if (list > shown && shown > 0) {
        float y0 = rowY(pinned), h = rowY(nvis - 1) + ROW_H - y0;
        C2D_DrawRectSolid(BOT_W - 6, y0, 0, 2, h, T->dim);
        C2D_DrawRectSolid(BOT_W - 6, y0 + h * (vis[pinned] - pinned) / list, 0, 2, h * shown / list, T->grey);
    }
    fitTail(line, sizeof(line), rows[selected].desc, 0.46f, BOT_W - 2 * MARGIN);
    text(MARGIN, DESC_Y, 0.46f, T->grey, line);
    text(MARGIN, 212, 0.45f, T->grey, prompt);
}

int main(int argc, char* argv[]) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);  // memcard draws each glyph nine times
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(2048);
    measureBuf = C2D_TextBufNew(256);

    // HOME would suspend the app and drop every FTP socket; sleep on lid close would too.
    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);
    hidSetRepeatParameters(24, 5);   // frames: 400 ms, then every ~80 ms

    Screen s;
    int theme = loadTheme();
    T = &THEMES[theme];
    char syncMsg[96] = "Both ways with /3ds/relay/saves";
    char inboxMsg[96] = "Installs CIAs in /3ds/relay/inbox";
    char updateMsg[96] = "Checks GitHub for a newer build";
    char pageMsg[96] = "";
    char themeMsg[48], wifiRow[64], slotRow[3][48], slotDesc[3][48], clockDesc[80];
    // Jobs block: the row shows its "working" line for one frame, the job runs on the next.
    enum class Job { None, Sync, Inbox, Update, Check, Radio, Restore, Clock } job = Job::None;
    char clockMsg[96] = "";
    bool checkAfterClock = false;   // launch: a wrong clock fails TLS, so it is set before the update check
    // Wi-Fi connect runs on its own thread (~10 s); the page counts the seconds meanwhile.
    static volatile bool connBusy = false;
    static int connSlot = 0;
    static char connMsg[96];
    Thread connThread = nullptr;
    u64 connStart = 0;
    int jobSlot = 0;
    std::vector<SaveTitle> titles;
    std::vector<SaveBackup> backups;
    int titleSel = 0;          // Saves page row of the title whose backups show
    int confirmRow = -1;       // Backups row waiting for its second A
    char titleDesc[96];
    bool radioOn = true;   // nwm has no getter; Wi-Fi off from HOME before launch shows as on
    bool checked = false;   // the launch-time update check, once Wi-Fi is up
    void* socBuffer = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool socUp = socBuffer && socInit((u32*)socBuffer, SOC_BUFFERSIZE) == 0;
    bool socOk = false;
    if (socUp) {
        s32 prio = 0;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        site_init(true);
        socOk = ftp_start(prio + 1, "trey relay") == 0;
    }
    s.state = socOk ? State::Starting : State::NoSockets;
    bool netUp = netInit();
    wifiInit();
    wifiPoll(s.wifi);
    led_broadcast(LED_BROADCAST_OFF);   // a stream that ended without the plugin clearing it

    pollPower(s);
    // Saves sync once at launch; it reads only the card, so Wi-Fi does not matter.
    snprintf(syncMsg, sizeof(syncMsg), "Syncing saves");
    job = Job::Sync;
    // Backlights go off after IDLE_OFF frames with an FTP client and no input; any input or the
    // last client leaving brings them back.
    // Backlight state lives in site.c, shared with SITE LIGHT: a press in the app lights both again.
    const int IDLE_OFF = 20 * 60;
    int idleFrames = 0;
    bool idleDark = false, idleUsed = false;   // screens off by idle; idle already spent this quiet spell
    unsigned long long lastDone = 0;

    enum class Page { Main, Wifi, Saves, Backups } page = Page::Main;
    int selected = ITEM_BROADCAST, mainSelected = ITEM_BROADCAST;
    float light[VIS] = { 1.0f };
    static Row rows[MAX_ROWS];
    int rowCount = 0, pinned = 0, scroll = 0;
    int frame = 0, armedFrames = 0;
    bool quit = false;

    while (!quit && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        down |= hidKeysDownRepeat() & (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN);   // held = repeat, lists
        bool input = down || hidKeysHeld();
        idleFrames = input ? 0 : idleFrames + 1;
        if (input) idleUsed = false;
        bool lit = !site_light_is_off();
        if (lit) idleDark = false;
        if (!lit && down) {
            site_set_light(false);
            idleDark = false;
            down = 0;   // the press that wakes the screens does nothing else
        } else if (lit && !idleUsed && s.ftp.sessions > 0 && idleFrames > IDLE_OFF && s.state != State::Armed) {
            idleUsed = idleDark = site_set_light(true);
        } else if (idleDark && (s.ftp.sessions == 0 || s.state == State::Armed)) {
            site_set_light(false);
            idleDark = false;
        }

        // Rows for this frame's page; built before input so touch and A see the same list.
        groups = page == Page::Main ? MAIN_GROUPS : page == Page::Wifi ? WIFI_GROUPS : page == Page::Saves ? SAVES_GROUPS : 0;
        pinned = page == Page::Saves ? 1 : 0;
        if (page == Page::Saves) {
            rows[0] = { "Sync now", syncMsg, true, false };
            rowCount = 1;
            for (auto& t : titles) if (rowCount < MAX_ROWS) rows[rowCount++] = { t.name, titleDesc, true, false };
        } else if (page == Page::Backups) {
            rowCount = 0;
            for (auto& b : backups) if (rowCount < MAX_ROWS) rows[rowCount++] = { b.label, titleDesc, true, false };
            if (!rowCount) rows[rowCount++] = { "No backups yet", "One is kept each time a sync replaces a copy", false, false };
        } else if (page == Page::Main) {
            snprintf(themeMsg, sizeof(themeMsg), "%s, A for the next", T->name);
            snprintf(wifiRow, sizeof(wifiRow), "%s", s.wifi.ap ? s.wifi.ssid : "Not connected");
            const char* desc[ITEM_COUNT] = { "Arms Artic Base for Azahar", syncMsg, inboxMsg,
                                             pageMsg[0] ? pageMsg : wifiRow, updateMsg, themeMsg };
            for (int i = 0; i < ITEM_COUNT; i++) rows[i] = { ITEM_TITLE[i], desc[i], true, false };
            rowCount = ITEM_COUNT;
        } else {
            for (int i = 0; i < 3; i++) {
                const char* name = s.wifi.slot[i];
                bool here = s.wifi.ap & BIT(i + 1);
                bool known = !s.wifi.slotsRead || name[0];
                snprintf(slotRow[i], sizeof(slotRow[i]), "Connection %d%s%s", i + 1,
                         s.wifi.slotsRead ? "  " : "", s.wifi.slotsRead ? (name[0] ? name : "empty") : "");
                snprintf(slotDesc[i], sizeof(slotDesc[i]), "%s", here ? "Connected" : known ? "A to connect, about 10 s" : "Set it up in Internet settings");
                rows[i] = { slotRow[i], slotDesc[i], known && !here && radioOn && !connThread, here };
            }
            rows[WIFI_RADIO] = { radioOn ? "Turn Wi-Fi off" : "Turn Wi-Fi on", radioOn ? "FTP stops until Wi-Fi is back" : "Wi-Fi is off", !connThread, false };
            rows[WIFI_SETTINGS] = { "Internet settings", "Opens System Settings to add or change networks", !connThread, false };
            if (!clockMsg[0] || job != Job::Clock) {
                time_t now = time(NULL);
                struct tm tm;
                gmtime_r(&now, &tm);
                char when[32];
                strftime(when, sizeof(when), "%d %b %Y  %H:%M", &tm);
                snprintf(clockDesc, sizeof(clockDesc), "%s%s", when, s.ip ? ", A sets it from the internet" : ", needs Wi-Fi to set");
            }
            rows[WIFI_CLOCK] = { "Set clock", clockMsg[0] ? clockMsg : clockDesc, s.ip && !connThread, false };
            rowCount = WIFI_COUNT;
        }
        if (pageMsg[0] && page != Page::Main && selected < rowCount) rows[selected].desc = pageMsg;

        int activate = -1;
        if (s.state != State::Armed) {
            if (down & KEY_START) break;
            if (down & (KEY_DUP | KEY_CPAD_UP)) selected = (selected + rowCount - 1) % rowCount;
            if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) selected = (selected + 1) % rowCount;
            if (down & (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN)) { pageMsg[0] = 0; confirmRow = -1; }
            if (down & KEY_A) activate = selected;
            if (down & KEY_TOUCH) {
                touchPosition t;
                hidTouchRead(&t);
                if (page != Page::Main && t.py >= 205 && t.px < 200) down |= KEY_B;   // the B Back prompt
                for (int v = 0; v < VIS && v < rowCount; v++) {
                    int i = v < pinned ? v : pinned + scroll + (v - pinned);
                    if (i < rowCount && t.py >= rowY(v) && t.py < rowY(v) + ROW_H) {
                        if (i != selected) confirmRow = -1;
                        selected = i;
                        activate = i;
                    }
                }
            }
            if ((down & KEY_B) && page != Page::Main) {
                if (page == Page::Backups) { page = Page::Saves; selected = titleSel; }
                else { page = Page::Main; selected = mainSelected; }
                pageMsg[0] = 0;
                confirmRow = -1;
                activate = -1;
            }
        }
        if (activate >= 0 && !rows[activate].enabled) activate = -1;

        if (job != Job::None) {
            Job j = job;
            job = Job::None;
            if (j == Job::Sync) {
                syncSaves(syncMsg, sizeof(syncMsg));
                if (page == Page::Saves) titles = saveTitles();
            } else if (j == Job::Restore) {
                const SaveTitle& t = titles[titleSel - 1];
                restoreBackup(t, backups[jobSlot], pageMsg, sizeof(pageMsg));
                backups = saveBackups(t);
                if (page == Page::Backups) selected = 0;   // the copy it replaced, now the newest
            } else if (j == Job::Inbox) {
                installInbox(inboxMsg, sizeof(inboxMsg));
            } else if (j == Job::Clock) {
                clockSync(clockMsg, sizeof(clockMsg));
                if (checkAfterClock) { checkAfterClock = false; job = Job::Check; }
            } else if (j == Job::Radio) {
                if (wifiRadio(!radioOn, pageMsg, sizeof(pageMsg))) radioOn = !radioOn;
            } else if (!netUp) {
                snprintf(updateMsg, sizeof(updateMsg), "HTTP service unavailable");
            } else if (j == Job::Check) {
                checkUpdate(updateMsg, sizeof(updateMsg));
                ftp_log("update check: %s", updateMsg);
            } else if (runUpdate(updateMsg, sizeof(updateMsg)) != UpdateResult::Installed) {
                ftp_log("update: %s", updateMsg);
            } else {
                aptSetChainloaderToSelf();   // relaunch into the new build on exit
                quit = true;
            }
        }

        u64 title = 0;
        u8 media = 0;
        site_req_t req = site_take(&title, &media);
        // SITE verbs act on main-page rows wherever the menu is.
        int remote = req == SITE_INBOX ? ITEM_INBOX : req == SITE_UPDATE ? ITEM_UPDATE : -1;
        int mainAct = page == Page::Main ? activate : -1;
        if (remote >= 0) mainAct = remote;
        if (req == SITE_SYNC || (page == Page::Saves && activate == 0)) { snprintf(syncMsg, sizeof(syncMsg), "Syncing saves"); job = Job::Sync; }
        if (mainAct == ITEM_SAVES) {
            mainSelected = ITEM_SAVES;
            page = Page::Saves;
            titles = saveTitles();
            selected = scroll = 0;
        } else if (page == Page::Saves && activate > 0) {
            titleSel = activate;
            backups = saveBackups(titles[activate - 1]);
            page = Page::Backups;
            selected = scroll = 0;
            confirmRow = -1;
        } else if (page == Page::Backups && activate >= 0 && job == Job::None) {
            if (confirmRow != activate) {
                confirmRow = activate;
                snprintf(pageMsg, sizeof(pageMsg), "A again restores this over the console's %s", backups[activate].ext ? "extdata" : "save");
            } else {
                confirmRow = -1;
                jobSlot = activate;
                snprintf(pageMsg, sizeof(pageMsg), "Restoring");
                job = Job::Restore;
            }
        }
        if (mainAct == ITEM_INBOX) { snprintf(inboxMsg, sizeof(inboxMsg), "Installing from inbox"); job = Job::Inbox; }
        if (mainAct == ITEM_UPDATE) { snprintf(updateMsg, sizeof(updateMsg), "Checking GitHub"); job = Job::Update; }
        if (mainAct == ITEM_THEME) {
            theme = (theme + 1) % THEME_COUNT;
            T = &THEMES[theme];
            saveTheme(theme);
        }
        if (mainAct == ITEM_WIFI && remote < 0) {
            mainSelected = ITEM_WIFI;
            page = Page::Wifi;
            wifiReadSlots(s.wifi);
            selected = 0;
            for (int i = 0; i < 3; i++) if (s.wifi.ap & BIT(i + 1)) selected = i;
            pageMsg[0] = 0;
        }
        if (mainAct == ITEM_BROADCAST) {
            syncSaves(syncMsg, sizeof(syncMsg));
            s.fault = armBroadcast();
            if (!s.fault) { s.state = State::Armed; led_broadcast(LED_BROADCAST_WAIT); }
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
        }
        if (page == Page::Wifi && remote < 0 && activate >= 0) {
            if (activate <= WIFI_SLOT3 && !connThread) {
                connSlot = activate;
                connBusy = true;
                connStart = osGetTime();
                s32 prio = 0x30;
                svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
                connThread = threadCreate([](void*) { wifiConnect(connSlot, connMsg, sizeof(connMsg)); connBusy = false; },
                                          nullptr, 0x4000, prio + 1, -1, false);
                if (!connThread) { connBusy = false; snprintf(pageMsg, sizeof(pageMsg), "Could not start connecting"); }
            } else if (activate == WIFI_CLOCK) {
                snprintf(clockMsg, sizeof(clockMsg), "Asking pool.ntp.org");
                job = Job::Clock;
            } else if (activate == WIFI_RADIO) {
                snprintf(pageMsg, sizeof(pageMsg), radioOn ? "Turning Wi-Fi off" : "Turning Wi-Fi on");
                job = Job::Radio;
            } else if (activate == WIFI_SETTINGS) {
                u64 tid = settingsTitle();
                if (tid) { aptSetChainloader(tid, MEDIATYPE_NAND); quit = true; }
                else snprintf(pageMsg, sizeof(pageMsg), "System Settings not found for this region");
            }
        }
        if (req == SITE_CLOCK) { snprintf(clockMsg, sizeof(clockMsg), "Asking pool.ntp.org"); job = Job::Clock; }
        if (connThread) {
            if (connBusy) {
                snprintf(pageMsg, sizeof(pageMsg), "Connecting to connection %d, %llu s", connSlot + 1, (osGetTime() - connStart) / 1000);
            } else {
                threadJoin(connThread, U64_MAX);
                threadFree(connThread);
                connThread = nullptr;
                snprintf(pageMsg, sizeof(pageMsg), "%s", connMsg);
                wifiPoll(s.wifi);
            }
            if (page == Page::Wifi && connThread) selected = connSlot;
        }
        if (req == SITE_LAUNCH) {
            // On exit libctru jumps to the chainload title, with the plugin armed for it.
            syncSaves(syncMsg, sizeof(syncMsg));
            s.fault = armBroadcast();
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
            if (!s.fault) { led_broadcast(LED_BROADCAST_WAIT); aptSetChainloader(title, media); quit = true; }
        } else if (req == SITE_REBOOT || req == SITE_POWEROFF) {
            svcSleepThread(300000000LL);  // the 200 reply leaves before the network goes
            site_power(req);
        }

        // Network state once a second: Wi-Fi can drop and return while the app sits here.
        if (socOk && s.state != State::Armed && frame % 60 == 0) {
            s.ip = (u32)gethostid();
            s.state = s.ip ? State::Ready : State::NoWifi;
            wifiPoll(s.wifi);
            if (frame % 300 == 0) pollPower(s);
            s.rate = s.ftp.xfer && s.ftp.xfer_done > lastDone ? s.ftp.xfer_done - lastDone : 0;
            lastDone = s.ftp.xfer ? s.ftp.xfer_done : 0;
            if (s.ip && !checked && job == Job::None) {
                checked = true;
                if (clockWrong()) { checkAfterClock = true; snprintf(clockMsg, sizeof(clockMsg), "Clock wrong, asking pool.ntp.org"); job = Job::Clock; }
                else job = Job::Check;
            }
        }
        if (s.state != State::Armed) ftp_status(&s.ftp);

        // Window over the list: the pinned rows, then the part holding the selection.
        int win = VIS - pinned;
        if (selected >= pinned) {
            if (selected - pinned < scroll) scroll = selected - pinned;
            if (selected - pinned >= scroll + win) scroll = selected - pinned - win + 1;
        }
        int maxScroll = rowCount - pinned - win;
        scroll = scroll > maxScroll ? (maxScroll > 0 ? maxScroll : 0) : scroll;
        int vis[VIS], nvis = 0;
        for (int v = 0; v < VIS; v++) {
            int i = v < pinned ? v : pinned + scroll + (v - pinned);
            if (i < rowCount) vis[nvis++] = i;
        }
        // Row description for the selected title or backup, worked out after this frame's input.
        const SaveTitle* curTitle = nullptr;
        if (page == Page::Saves && selected > 0 && selected <= (int)titles.size()) curTitle = &titles[selected - 1];
        if (page == Page::Backups && titleSel > 0 && titleSel <= (int)titles.size()) curTitle = &titles[titleSel - 1];
        const SaveBackup* curBackup = page == Page::Backups && selected < (int)backups.size() ? &backups[selected] : nullptr;
        if (page == Page::Saves) snprintf(titleDesc, sizeof(titleDesc), "A for its backups");
        if (curBackup) snprintf(titleDesc, sizeof(titleDesc), "%s",
                                curBackup->checkpoint ? "From Checkpoint. A restores it" : "A restores it, the console's copy is kept");

        // Selection light crossfades in place, about 150 ms.
        for (int v = 0; v < VIS; v++) {
            float target = (v < nvis && vis[v] == selected && s.state != State::Armed) ? 1.0f : 0.0f;
            light[v] += (target - light[v]) * 0.3f;
        }

        C2D_TextBufClear(textBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, T->field[0]);
        C2D_SceneBegin(top);
        field(T->field, TOP_W);
        if (page == Page::Wifi && s.state != State::Armed) drawWifiTop(s);
        else if ((page == Page::Saves || page == Page::Backups) && s.state != State::Armed)
            drawSavesTop(s, curTitle, titles.size(), curBackup, backups.size());
        else drawTop(s);
        C2D_TargetClear(bottom, T->field[0]);
        C2D_SceneBegin(bottom);
        field(T->bottom, BOT_W);
        // U+E000 A, U+E001 B in the system font.
        drawBottom(rows, rowCount, pinned, vis, nvis, selected, light, s.state == State::Armed,
                   page != Page::Main ? "\uE000 Select      \uE001 Back      START Exit" : "\uE000 Select      START Exit");
        C3D_FrameEnd(0);

        frame++;
        // Long enough to read the armed screen, then hand back to HOME.
        if (s.state == State::Armed && ++armedFrames > 120) break;
    }

    if (site_light_is_off()) site_set_light(false);
    if (connThread) { threadJoin(connThread, U64_MAX); threadFree(connThread); }
    ftp_stop();
    wifiExit();
    if (netUp) netExit();
    if (socUp) socExit();
    free(socBuffer);

    C2D_TextBufDelete(measureBuf);
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
