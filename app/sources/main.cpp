#include <3ds.h>
#include <citro2d.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm.hpp"
#include "knave.hpp"
#include "net.hpp"
#include "saves.hpp"
#include "update.hpp"
#include "ftp.h"
#include "site.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

// Themes match the Steam art's (Code/atlas/steam/art/art.html). mono: matte near-black field, white
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
      C2D_Color32(236, 236, 238, 255), C2D_Color32(150, 150, 156, 255), C2D_Color32(96, 96, 102, 255),
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
// Touch hits the whole row.
static const int ROW_TOP = 8, ROW_H = 28, DESC_Y = 182;
enum { ITEM_BROADCAST, ITEM_SYNC, ITEM_INBOX, ITEM_UPDATE, ITEM_THEME, ITEM_EXIT, ITEM_COUNT };
static const char* ITEM_TITLE[] = { "Broadcast a game", "Sync saves", "Install from knave", "Update", "Theme", "Exit" };
static int rowY(int i) { return ROW_TOP + i * ROW_H; }

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

// Fits str into max_w at scale by cutting from the front, keeping the file name end.
static void fitTail(char* out, size_t cap, const char* str, float scale, float max_w) {
    snprintf(out, cap, "%s", str);
    const char* p = str;
    for (;;) {
        C2D_Text t;
        C2D_TextBufClear(measureBuf);
        C2D_TextParse(&t, measureBuf, out);
        float w = 0;
        C2D_TextGetDimensions(&t, scale, scale, &w, nullptr);
        if (w <= max_w || !*p) return;
        p++;
        snprintf(out, cap, "...%s", p);
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

enum class State { Starting, Ready, NoWifi, NoSockets, Armed };

struct Screen {
    State state = State::Starting;
    u32 ip = 0;
    const char* fault = nullptr;  // arm failure, shown until the next attempt
    ftp_status_t ftp = {};
};

static void drawTop(const Screen& s) {
    char line[160];
    text(MARGIN, 16, 0.55f, T->grey, "Relay");
    snprintf(line, sizeof(line), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    text(TOP_W - MARGIN, 16, 0.5f, T->dim, line, C2D_AlignRight);

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
    if (s.ftp.sessions > 0) snprintf(line, sizeof(line), "%d connected", s.ftp.sessions);
    else snprintf(line, sizeof(line), "Idle");
    text(MARGIN, 152, 0.55f, T->grey, line);

    if (s.ftp.last[0]) {
        fitTail(line, sizeof(line), s.ftp.last, 0.5f, TOP_W - 2 * MARGIN);
        text(MARGIN, 176, 0.5f, T->dim, line);
    }
    if (s.ftp.rx || s.ftp.tx) {
        char rx[24], tx[24];
        formatBytes(rx, sizeof(rx), s.ftp.rx);
        formatBytes(tx, sizeof(tx), s.ftp.tx);
        snprintf(line, sizeof(line), "received %s    sent %s", rx, tx);
        text(MARGIN, 208, 0.45f, T->dim, line);
    }
}

static void drawBottom(int selected, const float* light, bool locked, const char* const* sub) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (!T->outline) streak(rowY(i), ROW_H, light[i]);  // PS2 browser marks selection by colour only
        bool on = i == selected && !locked;
        text(MARGIN, rowY(i) + 5, 0.58f, on ? T->sel : T->idle, ITEM_TITLE[i]);
    }
    char line[128];
    fitTail(line, sizeof(line), sub[selected], 0.46f, BOT_W - 2 * MARGIN);
    text(MARGIN, DESC_Y, 0.46f, T->grey, line);
    // U+E000 is the A button glyph in the system font.
    text(MARGIN, 212, 0.45f, T->dim, " Select      START Exit");
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

    Screen s;
    int theme = loadTheme();
    T = &THEMES[theme];
    char syncMsg[96] = "Both ways with Azahar on knave";
    char inboxMsg[96] = "Installs CIAs from knave's inbox";
    char updateMsg[96] = "Checks GitHub for a newer build";
    char themeMsg[48];
    // Network jobs block: the row shows its "working" line for one frame, the job runs on the next.
    enum class Job { None, Sync, Inbox, Update, Check } job = Job::None;
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

    int selected = ITEM_BROADCAST;
    float light[ITEM_COUNT] = { 1.0f };
    int frame = 0, armedFrames = 0;
    bool quit = false;

    while (!quit && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        int activate = -1;

        if (s.state != State::Armed) {
            if (down & KEY_START) break;
            if (down & (KEY_DUP | KEY_CPAD_UP)) selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
            if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) selected = (selected + 1) % ITEM_COUNT;
            if (down & KEY_A) activate = selected;
            if (down & KEY_TOUCH) {
                touchPosition t;
                hidTouchRead(&t);
                for (int i = 0; i < ITEM_COUNT; i++)
                    if (t.py >= rowY(i) && t.py < rowY(i) + ROW_H) { selected = i; activate = i; }
            }
        }

        if (activate == ITEM_EXIT) quit = true;
        if (activate == ITEM_THEME) {
            theme = (theme + 1) % THEME_COUNT;
            T = &THEMES[theme];
            saveTheme(theme);
        }
        if (job != Job::None) {
            Job j = job;
            job = Job::None;
            if (!netUp) {
                snprintf(j == Job::Sync ? syncMsg : j == Job::Inbox ? inboxMsg : updateMsg, 96, "HTTP service unavailable");
            } else if (j == Job::Sync) {
                syncSaves(syncMsg, sizeof(syncMsg));
            } else if (j == Job::Inbox) {
                installInbox(inboxMsg, sizeof(inboxMsg));
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
        if (req == SITE_SYNC) activate = ITEM_SYNC;
        if (req == SITE_INBOX) activate = ITEM_INBOX;
        if (req == SITE_UPDATE) activate = ITEM_UPDATE;
        if (activate == ITEM_SYNC) { snprintf(syncMsg, sizeof(syncMsg), "Syncing saves"); job = Job::Sync; }
        if (activate == ITEM_INBOX) { snprintf(inboxMsg, sizeof(inboxMsg), "Installing from knave"); job = Job::Inbox; }
        if (activate == ITEM_UPDATE) { snprintf(updateMsg, sizeof(updateMsg), "Checking GitHub"); job = Job::Update; }
        if (activate == ITEM_BROADCAST) {
            s.fault = armBroadcast();
            if (!s.fault) { s.state = State::Armed; if (netUp) wakeAzahar(); }
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
        }
        if (req == SITE_LAUNCH) {
            // On exit libctru jumps to the chainload title, with the plugin armed for it.
            s.fault = armBroadcast();
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
            if (!s.fault) { if (netUp) wakeAzahar(); aptSetChainloader(title, media); quit = true; }
        } else if (req == SITE_REBOOT || req == SITE_POWEROFF) {
            svcSleepThread(300000000LL);  // the 200 reply leaves before the network goes
            site_power(req);
        }

        // Network state once a second: Wi-Fi can drop and return while the app sits here.
        if (socOk && s.state != State::Armed && frame % 60 == 0) {
            s.ip = (u32)gethostid();
            s.state = s.ip ? State::Ready : State::NoWifi;
            if (s.ip && !checked && job == Job::None) { checked = true; job = Job::Check; }
        }
        if (s.state != State::Armed) ftp_status(&s.ftp);

        // Selection light crossfades in place, about 150 ms.
        for (int i = 0; i < ITEM_COUNT; i++) {
            float target = (i == selected && s.state != State::Armed) ? 1.0f : 0.0f;
            light[i] += (target - light[i]) * 0.3f;
        }

        C2D_TextBufClear(textBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        snprintf(themeMsg, sizeof(themeMsg), "%s, A for the next", T->name);
        const char* sub[ITEM_COUNT] = { "Arms Artic Base, wakes Azahar", syncMsg, inboxMsg,
                                        updateMsg, themeMsg, "Stops FTP" };
        C2D_TargetClear(top, T->field[0]);
        C2D_SceneBegin(top);
        field(T->field, TOP_W);
        drawTop(s);
        C2D_TargetClear(bottom, T->field[0]);
        C2D_SceneBegin(bottom);
        field(T->bottom, BOT_W);
        drawBottom(selected, light, s.state == State::Armed, sub);
        C3D_FrameEnd(0);

        frame++;
        // Long enough to read the armed screen, then hand back to HOME.
        if (s.state == State::Armed && ++armedFrames > 120) break;
    }

    ftp_stop();
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
