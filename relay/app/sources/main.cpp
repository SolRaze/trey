#include <3ds.h>
#include <citro2d.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "arm.hpp"
#include "clock.hpp"
#include "inbox.hpp"
#include "net.hpp"
#include "saves.hpp"
#include "titles.hpp"
#include "update.hpp"
#include "wifi.hpp"
#include "ftp.h"
#include "site.h"
#include "tunnel.h"
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

// Bottom screen menu: one line per row, the selected row's description or result below them. Main and
// Settings draw as icon tiles instead, see drawTiles.
// Touch hits the whole row. Pages: main; Saves, Titles and Settings under it; Backups under Saves; Wi-Fi
// under Settings (B back). At most VIS rows show; longer lists scroll under their first PINNED rows.
static const int ROW_TOP = 8, ROW_H = 25, GROUP_GAP = 6, DESC_Y = 182, VIS = 6, MAX_ROWS = 128;
enum { ITEM_BROADCAST, ITEM_SAVES, ITEM_TITLES, ITEM_INBOX, ITEM_SETTINGS, ITEM_COUNT };
static const char* ITEM_TITLE[] = { "Broadcast a game", "Saves", "Titles", "Inbox", "Settings" };
enum { SET_WIFI, SET_UPDATE, SET_THEME, SET_CLOCK, SET_COUNT };
static const char* SET_TITLE[] = { "Wi-Fi", "Update", "Theme", "Clock" };
enum { WIFI_SLOT1, WIFI_SLOT2, WIFI_SLOT3, WIFI_RADIO, WIFI_SETTINGS, WIFI_COUNT };
// Bit i set = a group starts at visible row i. Wi-Fi: the three connections | radio, settings.
// Saves: Sync now | titles.
static const u32 WIFI_GROUPS = BIT(WIFI_RADIO), SAVES_GROUPS = BIT(1);
static u32 groups = 0;
static int rowY(int i) {
    int y = ROW_TOP + i * ROW_H;
    for (int k = 1; k <= i; k++) if (groups & BIT(k)) y += GROUP_GAP;
    return y;
}

struct Row { const char* title; const char* desc; bool enabled; bool mark; int icon = -1; };   // icon: tile pages

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
    int tunnel = TUNNEL_OFF;
    char tunnelLine[48] = "";
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

    if (s.tunnel != TUNNEL_OFF)
        text(TOP_W - MARGIN, 152, 0.5f, s.tunnel == TUNNEL_UP ? T->white : T->grey, s.tunnelLine, C2D_AlignRight);
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
        snprintf(line, sizeof(line), "Never synced");
    }
    text(MARGIN, 152, 0.55f, T->grey, line);
    text(MARGIN, 176, 0.5f, T->dim, t->ext ? "Save and extdata" : "Save");
}

// Selected item's description or result, then the button prompt.
static void footer(const char* desc, const char* prompt) {
    char line[128];
    fitTail(line, sizeof(line), desc, 0.46f, BOT_W - 2 * MARGIN);
    text(MARGIN, DESC_Y, 0.46f, T->grey, line);
    text(MARGIN, 212, 0.45f, T->grey, prompt);
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
    footer(rows[selected].desc, prompt);
}

// Tile pages, Main and Settings: a 2-column grid of icon tiles, row-major, so each page fits without
// scrolling. Main keeps Broadcast as a full row above its grid; Settings has a page title there instead.
// tileAt and the d-pad moves read this layout.
static const int GRID_X = 16, GRID_Y = 42, TILE_GAP = 6, GRID_COLS = 2, TILE_H = 60;
static const int TILE_W = (BOT_W - 2 * GRID_X - (GRID_COLS - 1) * TILE_GAP) / GRID_COLS;
// k: index into the page's tiles, row 0 of the grid first.
static void tileXY(int k, int* x, int* y) {
    *x = GRID_X + (k % GRID_COLS) * (TILE_W + TILE_GAP);
    *y = GRID_Y + (k / GRID_COLS) * (TILE_H + TILE_GAP);
}
// Row under a touch on a tile page, -1 for none. first: rows before the tiles, 1 on Main.
static int tileAt(int first, int count, int px, int py) {
    if (first && py >= rowY(0) && py < rowY(0) + ROW_H) return 0;
    for (int i = first; i < count; i++) {
        int x, y;
        tileXY(i - first, &x, &y);
        if (px >= x && px < x + TILE_W && py >= y && py < y + TILE_H) return i;
    }
    return -1;
}

// Icon strokes. memcard borders them in black like its glyphs.
static void ink(float x, float y, float w, float h, u32 c) {
    if (T->outline) C2D_DrawRectSolid(x - 1, y - 1, 0, w + 2, h + 2, C2D_Color32(0, 0, 0, 255));
    C2D_DrawRectSolid(x, y, 0, w, h, c);
}
static void inkTri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
    if (T->outline) {
        u32 k = C2D_Color32(0, 0, 0, 255);
        float cx = (x0 + x1 + x2) / 3, cy = (y0 + y1 + y2) / 3, g = 1.3f;
        C2D_DrawTriangle(cx + (x0 - cx) * g, cy + (y0 - cy) * g, k, cx + (x1 - cx) * g, cy + (y1 - cy) * g, k,
                         cx + (x2 - cx) * g, cy + (y2 - cy) * g, k, 0);
    }
    C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0);
}
// Arc of radius r from angle a0 sweeping `sweep` radians clockwise. Every border before any colour, else
// each segment's border cuts the one drawn before it.
static void arc(float cx, float cy, float r, float a0, float sweep, u32 c) {
    const int N = 12;
    for (int pass = T->outline ? 0 : 1; pass < 2; pass++) {
        u32 k = pass ? c : C2D_Color32(0, 0, 0, 255);
        for (int j = 0; j < N; j++) {
            float a = a0 - sweep * j / N, b = a0 - sweep * (j + 1) / N;
            C2D_DrawLine(cx + r * cosf(a), cy - r * sinf(a), k, cx + r * cosf(b), cy - r * sinf(b), k, pass ? 2 : 4, 0);
        }
    }
}

enum { IC_NONE = -1, IC_SAVES, IC_TITLES, IC_INBOX, IC_SETTINGS, IC_WIFI, IC_UPDATE, IC_THEME, IC_CLOCK };

// 24x24 icons drawn from primitives at (x, y).
static void icon(int id, float x, float y, u32 c, int strength) {
    switch (id) {
    case IC_SAVES:      // memory card: frame and label
        ink(x + 4, y + 1, 16, 2, c); ink(x + 4, y + 21, 16, 2, c);
        ink(x + 4, y + 1, 2, 22, c); ink(x + 18, y + 1, 2, 22, c);
        ink(x + 8, y + 5, 8, 6, c);
        break;
    case IC_TITLES:     // four app squares
        for (int k = 0; k < 4; k++) ink(x + 2 + (k % 2) * 11, y + 2 + (k / 2) * 11, 9, 9, c);
        break;
    case IC_INBOX:      // arrow into a tray
        ink(x + 11, y + 1, 2, 9, c);
        inkTri(x + 6, y + 9, x + 18, y + 9, x + 12, y + 16, c);
        ink(x + 2, y + 21, 20, 2, c); ink(x + 2, y + 14, 2, 9, c); ink(x + 20, y + 14, 2, 9, c);
        break;
    case IC_SETTINGS:   // three sliders
        for (int k = 0; k < 3; k++) ink(x + 2, y + 4 + k * 8, 20, 2, c);
        ink(x + 14, y + 2, 4, 6, c); ink(x + 5, y + 10, 4, 6, c); ink(x + 11, y + 18, 4, 6, c);
        break;
    case IC_WIFI:       // HOME Menu bars, lit by the live strength
        for (int k = 0; k < 3; k++) ink(x + 3 + k * 7, y + 16 - k * 6, 5, 7 + k * 6, k < strength ? c : T->dim);
        break;
    case IC_UPDATE: {   // circular arrow, head at the open end pointing clockwise
        const float r = 9, cx = x + 12, cy = y + 12, a0 = 0.7f;
        arc(cx, cy, r, a0 + 2 * M_PI - 1.4f, 2 * M_PI - 1.4f, c);
        float ex = cx + r * cosf(a0), ey = cy - r * sinf(a0);
        inkTri(ex - 5, ey - 2, ex + 5, ey - 2, ex, ey + 5, c);
        break;
    }
    case IC_THEME:      // the theme's own swatches
        ink(x + 2, y + 4, 6, 16, T->white); ink(x + 9, y + 4, 6, 16, T->live); ink(x + 16, y + 4, 6, 16, T->sel);
        break;
    case IC_CLOCK:      // face and two hands
        arc(x + 12, y + 12, 10, 0, 2 * M_PI, c);
        ink(x + 11, y + 5, 2, 8, c); ink(x + 11, y + 11, 6, 2, c);
        break;
    }
}

// first: 1 when a full row (Broadcast) sits above the tiles, 0 when the page title does.
static void drawTiles(const Row* rows, int count, int first, const char* pageTitle, int selected, const float* light,
                      bool locked, const char* prompt, int strength) {
    char line[128];
    if (!first) text(MARGIN, rowY(0) + 5, 0.58f, T->grey, pageTitle);
    for (int i = 0; i < count; i++) {
        const Row& r = rows[i];
        bool on = i == selected && !locked;
        u32 c = on ? T->sel : r.enabled ? T->idle : T->dim;
        if (i < first) {
            if (!T->outline) streak(rowY(0), ROW_H, light[0]);
            fitHead(line, sizeof(line), r.title, 0.58f, BOT_W - 2 * MARGIN);
            text(MARGIN, rowY(0) + 5, 0.58f, c, line);
            continue;
        }
        int x, y;
        tileXY(i - first, &x, &y);
        // Tile face: faint in both themes so it reads as a button; mono lights the selected one.
        C2D_DrawRectSolid(x, y, 0, TILE_W, TILE_H, T->outline ? C2D_Color32(0, 0, 0, 40) : C2D_Color32(255, 255, 255, 8));
        if (!T->outline && light[i] > 0.01f) {
            u32 face = C2D_Color32(255, 255, 255, (u8)(light[i] * 30)), edge = C2D_Color32(255, 255, 255, (u8)(light[i] * 70));
            C2D_DrawRectSolid(x, y, 0, TILE_W, TILE_H, face);
            C2D_DrawRectSolid(x, y, 0, TILE_W, 1, edge);
            C2D_DrawRectSolid(x, y + TILE_H - 1, 0, TILE_W, 1, edge);
            C2D_DrawRectSolid(x, y, 0, 1, TILE_H, edge);
            C2D_DrawRectSolid(x + TILE_W - 1, y, 0, 1, TILE_H, edge);
        }
        icon(r.icon, x + (TILE_W - 24) / 2, y + 9, c, strength);
        fitHead(line, sizeof(line), r.title, 0.5f, TILE_W - 8);
        text(x + TILE_W / 2, y + 38, 0.5f, c, line, C2D_AlignCenter);
    }
    footer(rows[selected].desc, prompt);
}

// Jobs run one at a time on a worker thread below FTP and the tunnel, so the menu keeps drawing and
// a client keeps its transfer. A worker job only writes its own message line; anything that reads or
// changes menu state happens on the main thread once it ends (finishJob).
enum class Job { Sync, Inbox, Update, Check, Radio, Restore, Clock };
struct JobCtx {
    Job kind;
    volatile bool busy;
    bool ok;
    bool netUp;
    bool radioOn;                 // Radio: the state before the toggle
    SaveTitle title;              // Restore: copies, the menu may relist under it
    SaveBackup backup;
    UpdateResult update;
    char *syncMsg, *inboxMsg, *updateMsg, *clockMsg, *pageMsg;
};
static JobCtx jc;

static void runJob(void*) {
    switch (jc.kind) {
    case Job::Sync:    jc.ok = syncSaves(jc.syncMsg, 96); break;
    case Job::Restore: jc.ok = restoreBackup(jc.title, jc.backup, jc.pageMsg, 96); break;
    case Job::Inbox:   jc.ok = installInbox(jc.inboxMsg, 96); break;
    case Job::Clock:   jc.ok = clockSync(jc.clockMsg, 96); break;
    case Job::Radio:   jc.ok = wifiRadio(!jc.radioOn, jc.pageMsg, 96); break;
    case Job::Check:
    case Job::Update:
        if (!jc.netUp) { snprintf(jc.updateMsg, 96, "HTTP service unavailable"); jc.update = UpdateResult::Failed; break; }
        if (jc.kind == Job::Check) { jc.update = checkUpdate(jc.updateMsg, 96); ftp_log("update check: %s", jc.updateMsg); break; }
        jc.update = runUpdate(jc.updateMsg, 96);
        if (jc.update != UpdateResult::Installed) ftp_log("update: %s", jc.updateMsg);
        break;
    }
    jc.busy = false;
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
    // Queued in order, each kind once, so a SITE request and a row pressed in the same frame both run.
    std::vector<Job> jobs;
    Thread jobThread = nullptr;
    auto queue = [&](Job j) { if (std::find(jobs.begin(), jobs.end(), j) == jobs.end()) jobs.push_back(j); };
    char clockMsg[96] = "";
    bool checkAfterClock = false;   // launch: a wrong clock fails TLS, so it is set before the update check
    // Wi-Fi connect runs on its own thread (~10 s); the page counts the seconds meanwhile.
    static volatile bool connBusy = false;
    static int connSlot = 0;
    static char connMsg[96];
    Thread connThread = nullptr;
    u64 connStart = 0;
    auto startConnect = [&](int slot) {
        connSlot = slot;
        connBusy = true;
        connStart = osGetTime();
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        connThread = threadCreate([](void*) { wifiConnect(connSlot, connMsg, sizeof(connMsg)); connBusy = false; },
                                  nullptr, 0x4000, prio + 1, -1, false);
        if (!connThread) { connBusy = false; snprintf(pageMsg, sizeof(pageMsg), "Could not start connecting"); }
    };
    int jobSlot = 0;
    std::vector<SaveTitle> titles;
    std::vector<SaveBackup> backups;
    int titleSel = 0;          // Saves page row of the title whose backups show
    int confirmRow = -1;       // Backups or Titles row waiting for its second A
    std::vector<AppTitle> apps;
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
        if (socOk) tunnel_start(prio + 1);
    }
    s.state = socOk ? State::Starting : State::NoSockets;
    bool netUp = netInit();
    wifiInit();
    wifiPoll(s.wifi);
    led_broadcast(LED_BROADCAST_OFF);   // a stream that ended without the plugin clearing it

    pollPower(s);
    logHeap();
    // Saves sync once at launch; it reads only the card, so Wi-Fi does not matter.
    snprintf(syncMsg, sizeof(syncMsg), "Syncing saves");
    queue(Job::Sync);
    // Backlights go off after IDLE_OFF frames with an FTP client and no input; any input or the
    // last client leaving brings them back.
    // Backlight state lives in site.c, shared with SITE LIGHT: a press in the app lights both again.
    const int IDLE_OFF = 20 * 60;
    int idleFrames = 0;
    bool idleDark = false, idleUsed = false;   // screens off by idle; idle already spent this quiet spell
    unsigned long long lastDone = 0;

    enum class Page { Main, Settings, Wifi, Saves, Backups, Titles } page = Page::Main;
    int selected = ITEM_BROADCAST, mainSelected = ITEM_BROADCAST, setSelected = SET_WIFI;
    const int SLOTS = ITEM_COUNT > VIS ? ITEM_COUNT : VIS;   // light per visible row, or per tile page item
    float light[SLOTS] = { 1.0f };
    static Row rows[MAX_ROWS];
    int rowCount = 0, pinned = 0, scroll = 0;
    int frame = 0, armedFrames = 0;
    bool quit = false;

    // Input on UDP 4950, the packet Rosalina's InputRedirection takes: u32 pad word, active low, 0xfff
    // idle, HID bit order; u32 touch, bit 24 = touching, y << 12 | x, both 12-bit; circle pad, c-stick
    // and HOME/POWER words follow, unused. Drives Relay's own pages; HOME and POWER do nothing here.
    // While Rosalina's InputRedirection holds the port the bind fails and is retried every 5 s; its
    // presses arrive through HID then. Non-blocking, read every frame on the main thread.
    const u16 IR_PORT = 4950;
    int irSock = -1;
    u32 irHeld = 0;
    bool irTouching = false;
    touchPosition irPos = {};
    auto irOpen = [&] {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(IR_PORT);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr*)&a, sizeof(a))) { close(fd); return; }
        irSock = fd;
        ftp_log("input: UDP %u", IR_PORT);
    };
    // Lid closed: both backlights off until it opens. Sleep stays disallowed, so FTP keeps running.
    bool lidShut = false, lidDark = false;
    ptmuInit();   // held for the lid poll; pollPower's own init and exit only count

    jc.syncMsg = syncMsg; jc.inboxMsg = inboxMsg; jc.updateMsg = updateMsg; jc.clockMsg = clockMsg; jc.pageMsg = pageMsg;
    // Menu follow-ups of the job that just ended, on the main thread.
    auto afterJob = [&] {
        switch (jc.kind) {
        case Job::Sync:    if (page == Page::Saves) titles = saveTitles(); break;
        case Job::Restore:
            for (auto& t : titles) if (t.tid == jc.title.tid) { backups = saveBackups(t); break; }
            if (page == Page::Backups) selected = 0;   // the copy it replaced, now the newest
            break;
        case Job::Clock:   if (checkAfterClock) { checkAfterClock = false; queue(Job::Check); } break;
        case Job::Radio:   if (jc.ok) radioOn = !radioOn; break;
        case Job::Update:
            if (jc.update == UpdateResult::Installed) { aptSetChainloaderToSelf(); quit = true; }   // relaunch into it
            break;
        case Job::Inbox: case Job::Check: break;
        }
    };
    auto finishJob = [&] {
        threadJoin(jobThread, U64_MAX);
        threadFree(jobThread);
        jobThread = nullptr;
        afterJob();
    };
    // Before anything on the main thread that syncs saves, installs or uninstalls.
    auto waitJob = [&] { if (jobThread) finishJob(); };

    while (!quit && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        down |= hidKeysDownRepeat() & (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_DLEFT | KEY_DRIGHT |
                                       KEY_CPAD_LEFT | KEY_CPAD_RIGHT);   // held = repeat, lists and tiles
        u32 irDown = 0;
        if (irSock < 0 && socOk && frame % 300 == 0) irOpen();
        if (irSock >= 0) {
            u8 p[20];
            // Each packet is a whole state; a press and release in one frame still counts as a press.
            while (recv(irSock, p, sizeof(p), 0) >= 8) {
                u32 pad = p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
                u32 touch = p[4] | p[5] << 8 | p[6] << 16 | (u32)p[7] << 24;
                u32 held = ~pad & 0xFFF;
                irDown |= held & ~irHeld;
                irHeld = held;
                bool t = touch & BIT(24);
                if (t && !irTouching) {
                    irDown |= KEY_TOUCH;
                    irPos.px = (touch & 0xFFF) * 320 / 4096;
                    irPos.py = ((touch >> 12) & 0xFFF) * 240 / 4096;
                }
                irTouching = t;
            }
        }
        down |= irDown;
        bool input = down || hidKeysHeld() || irHeld || irTouching;
        if (frame % 15 == 0) {
            u8 open = 1;
            if (R_SUCCEEDED(PTMU_GetShellState(&open)) && lidShut != !open) {
                lidShut = !open;
                // Dark by idle at close counts as the lid's: opening it lights the screens either way.
                if (lidShut) lidDark = site_light_is_off() ? idleDark : site_set_light(true);
                else if (lidDark) { site_set_light(false); lidDark = false; }
                idleDark = false;
            }
        }
        idleFrames = input ? 0 : idleFrames + 1;
        if (input) idleUsed = false;
        bool lit = !site_light_is_off();
        if (lit) idleDark = false;
        if (lidShut) {
            // Presses still act; the screens stay dark until the lid opens.
        } else if (!lit && down) {
            site_set_light(false);
            idleDark = false;
            down = 0;   // the press that wakes the screens does nothing else
        } else if (lit && !idleUsed && s.ftp.sessions > 0 && idleFrames > IDLE_OFF && s.state != State::Armed) {
            idleUsed = idleDark = site_set_light(true);
        } else if (idleDark && (s.ftp.sessions == 0 || s.state == State::Armed)) {
            site_set_light(false);
            idleDark = false;
        }

        // Rows for the current page. Built before input so touch and A see the same list, and again
        // before drawing: relisting titles, backups or apps frees the names the rows point at.
        auto buildRows = [&] {
            groups = page == Page::Wifi ? WIFI_GROUPS : page == Page::Saves ? SAVES_GROUPS : 0;
            pinned = page == Page::Saves ? 1 : 0;
            if (page == Page::Saves) {
                rows[0] = { "Sync now", syncMsg, true, false };
                rowCount = 1;
                for (auto& t : titles) if (rowCount < MAX_ROWS) rows[rowCount++] = { t.name, titleDesc, true, false };
            } else if (page == Page::Backups) {
                rowCount = 0;
                for (auto& b : backups) if (rowCount < MAX_ROWS) rows[rowCount++] = { b.label, titleDesc, true, false };
                if (!rowCount) rows[rowCount++] = { "No backups yet", "One is kept each time a sync replaces a copy", false, false };
            } else if (page == Page::Titles) {
                rowCount = 0;
                for (auto& t : apps) if (rowCount < MAX_ROWS) rows[rowCount++] = { t.name, titleDesc, !t.kept, false };
                if (!rowCount) rows[rowCount++] = { "No titles", "", false, false };
            } else if (page == Page::Main) {
                static const int ICON[ITEM_COUNT] = { IC_NONE, IC_SAVES, IC_TITLES, IC_INBOX, IC_SETTINGS };
                const char* desc[ITEM_COUNT] = { "Arms Artic Base for Azahar", syncMsg, "Uninstall what you no longer play", inboxMsg,
                                                 "Wi-Fi, update, theme, clock" };
                for (int i = 0; i < ITEM_COUNT; i++) rows[i] = { ITEM_TITLE[i], desc[i], true, false, ICON[i] };
                rowCount = ITEM_COUNT;
            } else if (page == Page::Settings) {
                static const int ICON[SET_COUNT] = { IC_WIFI, IC_UPDATE, IC_THEME, IC_CLOCK };
                snprintf(themeMsg, sizeof(themeMsg), "%s, A for the next", T->name);
                snprintf(wifiRow, sizeof(wifiRow), "%s", s.wifi.ap ? s.wifi.ssid : "Not connected");
                time_t now = time(NULL);
                struct tm tm;
                gmtime_r(&now, &tm);
                char when[32];
                strftime(when, sizeof(when), "%d %b %Y  %H:%M", &tm);
                snprintf(clockDesc, sizeof(clockDesc), "%s%s", when, s.ip ? ", A sets it from the internet" : ", needs Wi-Fi to set");
                const char* desc[SET_COUNT] = { pageMsg[0] ? pageMsg : wifiRow, updateMsg, themeMsg, clockMsg[0] ? clockMsg : clockDesc };
                for (int i = 0; i < SET_COUNT; i++) rows[i] = { SET_TITLE[i], desc[i], true, false, ICON[i] };
                rows[SET_CLOCK].enabled = s.ip && !connThread;
                rowCount = SET_COUNT;
            } else {
                for (int i = 0; i < 3; i++) {
                    const char* name = s.wifi.slot[i];
                    bool here = s.wifi.ap & BIT(i + 1);
                    bool known = !s.wifi.slotsRead || name[0];
                    snprintf(slotRow[i], sizeof(slotRow[i]), "Connection %d%s%s", i + 1,
                             s.wifi.slotsRead ? "  " : "", s.wifi.slotsRead ? (name[0] ? name : "empty") : "");
                    snprintf(slotDesc[i], sizeof(slotDesc[i]), "%s", here ? "Connected, Y changes it" : known ? "A connects, Y changes it" : "A sets it up");
                    rows[i] = { slotRow[i], slotDesc[i], !connThread && (!known || (!here && radioOn)), here };
                }
                rows[WIFI_RADIO] = { radioOn ? "Turn Wi-Fi off" : "Turn Wi-Fi on", radioOn ? "FTP stops until Wi-Fi is back" : "Wi-Fi is off", !connThread, false };
                rows[WIFI_SETTINGS] = { "Internet settings", "Opens System Settings for proxy and IP", !connThread, false };
                rowCount = WIFI_COUNT;
            }
            if (selected >= rowCount) selected = rowCount - 1;   // the list shrank under the selection
            if (pageMsg[0] && page != Page::Main && page != Page::Settings) rows[selected].desc = pageMsg;
        };
        buildRows();

        int activate = -1;
        if (s.state != State::Armed) {
            if (down & KEY_START) break;
            const u32 UP = KEY_DUP | KEY_CPAD_UP, DOWN = KEY_DDOWN | KEY_CPAD_DOWN;
            const u32 LEFT = KEY_DLEFT | KEY_CPAD_LEFT, RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT;
            int first = page == Page::Main ? 1 : 0;   // Broadcast row above Main's grid
            bool tiles = page == Page::Main || page == Page::Settings;
            if (tiles) {
                // Up from the top tile row returns to Broadcast, down from it enters the grid's first tile.
                int k = selected - first, col = k % GRID_COLS;
                if (selected < first) { if (down & DOWN) selected = first; }
                else if (down & UP) selected = k >= GRID_COLS ? selected - GRID_COLS : first ? 0 : selected;
                else if (down & DOWN) selected = selected + GRID_COLS < rowCount ? selected + GRID_COLS : selected;
                else if ((down & LEFT) && col > 0) selected--;
                else if ((down & RIGHT) && col < GRID_COLS - 1 && selected + 1 < rowCount) selected++;
            } else {
                if (down & UP) selected = (selected + rowCount - 1) % rowCount;
                if (down & DOWN) selected = (selected + 1) % rowCount;
            }
            if (down & (UP | DOWN | LEFT | RIGHT)) { pageMsg[0] = clockMsg[0] = 0; confirmRow = -1; }
            if (down & KEY_A) activate = selected;
            if (down & KEY_TOUCH) {
                touchPosition t;
                if (irDown & KEY_TOUCH) t = irPos;
                else hidTouchRead(&t);
                if (page != Page::Main && t.py >= 205 && t.px < 200) down |= KEY_B;   // the B Back prompt
                if (tiles) {
                    int i = tileAt(first, rowCount, t.px, t.py);
                    if (i >= 0) selected = activate = i;
                }
                for (int v = 0; v < VIS && v < rowCount && !tiles; v++) {
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
                else if (page == Page::Wifi) { page = Page::Settings; selected = setSelected; }
                else { page = Page::Main; selected = mainSelected; }
                pageMsg[0] = clockMsg[0] = 0;
                confirmRow = -1;
                activate = -1;
            }
        }
        if (activate >= 0 && !rows[activate].enabled) activate = -1;

        if (jobThread && !jc.busy) finishJob();
        if (!jobThread && !jobs.empty() && jobs.front() == Job::Restore &&
            (titleSel < 1 || titleSel > (int)titles.size() || jobSlot >= (int)backups.size())) {
            jobs.erase(jobs.begin());   // the lists it pointed into changed before it started
            snprintf(pageMsg, sizeof(pageMsg), "Restore dropped, open the backup again");
        }
        if (!jobThread && !jobs.empty()) {
            jc.kind = jobs.front();
            jobs.erase(jobs.begin());
            if (jc.kind == Job::Restore) { jc.title = titles[titleSel - 1]; jc.backup = backups[jobSlot]; }
            jc.netUp = netUp;
            jc.radioOn = radioOn;
            jc.busy = true;
            s32 prio = 0x30;
            svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
            // Below FTP and the tunnel (prio + 1): equal priorities never preempt each other on a core.
            jobThread = threadCreate(runJob, nullptr, 0x20000, prio + 2, -1, false);
            if (!jobThread) { runJob(nullptr); afterJob(); }   // no thread to be had: run it here
        }

        u64 title = 0;
        u8 media = 0;
        site_req_t req = site_take(&title, &media);
        // SITE verbs act on the Inbox tile and the Update tile wherever the menu is.
        int remote = req == SITE_INBOX ? ITEM_INBOX : -1;
        int mainAct = page == Page::Main ? activate : -1;
        int setAct = page == Page::Settings ? activate : -1;   // before any page change below
        if (remote >= 0) mainAct = remote;
        if (req == SITE_SYNC || (page == Page::Saves && activate == 0)) { snprintf(syncMsg, sizeof(syncMsg), "Syncing saves"); queue(Job::Sync); }
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
        } else if (mainAct == ITEM_TITLES && remote < 0) {
            mainSelected = ITEM_TITLES;
            page = Page::Titles;
            apps = appTitles();
            selected = scroll = 0;
            confirmRow = -1;
        } else if (page == Page::Titles && mainAct < 0 && activate >= 0 && activate < (int)apps.size() && jobs.empty() && !jobThread) {
            if (confirmRow != activate) {
                confirmRow = activate;
                snprintf(pageMsg, sizeof(pageMsg), "A again uninstalls %.40s", apps[activate].name);
            } else {
                confirmRow = -1;
                uninstallTitle(apps[activate].tid, pageMsg, sizeof(pageMsg));
                apps = appTitles();
                if (selected >= (int)apps.size()) selected = apps.empty() ? 0 : apps.size() - 1;
            }
        } else if (page == Page::Backups && activate >= 0 && jobs.empty() && !jobThread) {
            if (confirmRow != activate) {
                confirmRow = activate;
                snprintf(pageMsg, sizeof(pageMsg), "A again restores this over the console's %s", backups[activate].ext ? "extdata" : "save");
            } else {
                confirmRow = -1;
                jobSlot = activate;
                snprintf(pageMsg, sizeof(pageMsg), "Restoring");
                queue(Job::Restore);
            }
        }
        if (mainAct == ITEM_INBOX) { snprintf(inboxMsg, sizeof(inboxMsg), "Installing from inbox"); queue(Job::Inbox); }
        if (setAct == SET_UPDATE || req == SITE_UPDATE) { snprintf(updateMsg, sizeof(updateMsg), "Checking GitHub"); queue(Job::Update); }
        if (setAct == SET_CLOCK) { snprintf(clockMsg, sizeof(clockMsg), "Asking pool.ntp.org"); queue(Job::Clock); }
        if (mainAct == ITEM_SETTINGS) {
            mainSelected = ITEM_SETTINGS;
            page = Page::Settings;
            selected = setSelected;
            pageMsg[0] = 0;
        }
        if (setAct == SET_THEME) {
            theme = (theme + 1) % THEME_COUNT;
            T = &THEMES[theme];
            saveTheme(theme);
        }
        if (setAct == SET_WIFI) {
            setSelected = SET_WIFI;
            page = Page::Wifi;
            wifiReadSlots(s.wifi);
            selected = 0;
            for (int i = 0; i < 3; i++) if (s.wifi.ap & BIT(i + 1)) selected = i;
            pageMsg[0] = 0;
        }
        if (mainAct == ITEM_BROADCAST) {
            waitJob();
            syncSaves(syncMsg, sizeof(syncMsg));
            s.fault = armBroadcast();
            if (!s.fault) { s.state = State::Armed; led_broadcast(LED_BROADCAST_WAIT); }
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
        }
        // setAct < 0: the press that opened this page is not also a press on it.
        if (page == Page::Wifi && setAct < 0 && s.state != State::Armed && !connThread) {
            // A on an empty slot or Y on any slot: keyboard for name and password, then connect to it.
            int edit = -1;
            if ((down & KEY_Y) && selected <= WIFI_SLOT3) edit = selected;
            if (activate >= 0 && activate <= WIFI_SLOT3 && s.wifi.slotsRead && !s.wifi.slot[activate][0]) edit = activate;
            if (edit >= 0) activate = wifiEditSlot(edit, pageMsg, sizeof(pageMsg)) ? edit : -1;
            if (edit >= 0) wifiReadSlots(s.wifi);
        }
        if (page == Page::Wifi && setAct < 0 && activate >= 0) {
            if (activate <= WIFI_SLOT3 && !connThread) {
                startConnect(activate);
            } else if (activate == WIFI_RADIO) {
                snprintf(pageMsg, sizeof(pageMsg), radioOn ? "Turning Wi-Fi off" : "Turning Wi-Fi on");
                queue(Job::Radio);
            } else if (activate == WIFI_SETTINGS) {
                u64 tid = settingsTitle();
                if (tid) { aptSetChainloader(tid, MEDIATYPE_NAND); quit = true; }
                else snprintf(pageMsg, sizeof(pageMsg), "System Settings not found for this region");
            }
        }
        if (req == SITE_CLOCK) { snprintf(clockMsg, sizeof(clockMsg), "Asking pool.ntp.org"); queue(Job::Clock); }
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
            // Relay's own title only relaunches: the plugin never loads into it, so an arm would stay
            // pending and land in whatever title starts next.
            u64 self = 0;
            APT_GetProgramID(&self);
            waitJob();
            syncSaves(syncMsg, sizeof(syncMsg));
            s.fault = title == self ? nullptr : armBroadcast();
            if (title != self) ftp_log("arm: %s", s.fault ? s.fault : "ok");
            if (!s.fault) { if (title != self) led_broadcast(LED_BROADCAST_WAIT); aptSetChainloader(title, media); quit = true; }
        } else if (req == SITE_FETCH) {
            char m[96];
            waitJob();
            bool ok = netUp && installCia(site_text(0), m, sizeof(m));
            if (!netUp) snprintf(m, sizeof(m), "HTTP service unavailable");
            ftp_log("fetch %s: %s", site_text(0), ok ? "installed" : m);
        } else if (req == SITE_WIFI) {
            char m[96];
            if (connThread) ftp_log("wifi: a connect is running, try again");
            else if (wifiSaveSlot((int)title, site_text(0), site_text(1), m, sizeof(m))) { wifiReadSlots(s.wifi); startConnect((int)title); }
            else ftp_log("wifi: %s", m);
        } else if (req == SITE_UNINSTALL) {
            char m[96];
            waitJob();
            uninstallTitle(title, m, sizeof(m));
            ftp_log("%s", m);
            if (page == Page::Titles) { apps = appTitles(); selected = 0; confirmRow = -1; }
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
            if (s.ip && !checked && jobs.empty() && !jobThread) {
                checked = true;
                if (clockWrong()) { checkAfterClock = true; snprintf(clockMsg, sizeof(clockMsg), "Clock wrong, asking pool.ntp.org"); queue(Job::Clock); }
                else queue(Job::Check);
            }
        }
        if (s.state != State::Armed) ftp_status(&s.ftp);
        s.tunnel = tunnel_status(s.tunnelLine, sizeof(s.tunnelLine));

        buildRows();
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
        if (curBackup) snprintf(titleDesc, sizeof(titleDesc), "A restores it, the console's copy is kept");
        if (page == Page::Titles && selected < (int)apps.size()) {
            const AppTitle& t = apps[selected];
            snprintf(titleDesc, sizeof(titleDesc), "%llu MB, %s", (t.size + (1 << 19)) >> 20,
                     t.media != MEDIATYPE_SD ? "game card" : t.kept ? "kept, never uninstalled" : "A twice uninstalls, its save stays in relay");
        }

        // Selection light crossfades in place, about 150 ms.
        for (int v = 0; v < SLOTS; v++) {
            bool lit = page == Page::Main || page == Page::Settings ? v == selected : v < nvis && vis[v] == selected;
            float target = lit && s.state != State::Armed ? 1.0f : 0.0f;
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
        if (page == Page::Main || page == Page::Settings)
            drawTiles(rows, rowCount, page == Page::Main ? 1 : 0, "Settings", selected, light, s.state == State::Armed,
                      page == Page::Main ? "\uE000 Select      START Exit" : "\uE000 Select      \uE001 Back      START Exit",
                      s.wifi.ap ? s.wifi.strength : 0);
        else
            drawBottom(rows, rowCount, pinned, vis, nvis, selected, light, s.state == State::Armed,
                       "\uE000 Select      \uE001 Back      START Exit");
        C3D_FrameEnd(0);

        frame++;
        // Long enough to read the armed screen, then hand back to HOME.
        if (s.state == State::Armed && ++armedFrames > 120) break;
    }

    if (site_light_is_off()) site_set_light(false);
    waitJob();
    if (connThread) { threadJoin(connThread, U64_MAX); threadFree(connThread); }
    // Before ftp_stop: the tunnel's local connections are FTP sessions, and no thread may touch a socket after socExit.
    if (irSock >= 0) close(irSock);
    ptmuExit();
    tunnel_stop();
    bool ftpClear = ftp_stop();
    wifiExit();
    if (netUp) netExit();
    // A session still alive keeps soc and its buffer; process exit reclaims both.
    if (socUp && ftpClear) socExit();
    if (ftpClear) free(socBuffer);

    C2D_TextBufDelete(measureBuf);
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
