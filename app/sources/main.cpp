#include <3ds.h>
#include <citro2d.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm.hpp"
#include "ftp.h"
#include "site.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

// Matte near-black field, white type, one pale accent for live, red only for faults.
static const u32 BG     = C2D_Color32(12, 12, 14, 255);
static const u32 WHITE  = C2D_Color32(236, 236, 238, 255);
static const u32 GREY   = C2D_Color32(150, 150, 156, 255);
static const u32 DIM    = C2D_Color32(96, 96, 102, 255);
static const u32 LIVE   = C2D_Color32(158, 214, 190, 255);
static const u32 FAULT  = C2D_Color32(230, 104, 104, 255);

static const int TOP_W = 400, BOT_W = 320;
static const int MARGIN = 24;

// Bottom screen menu rows. Touch hits the whole row.
static const int ROW_Y[] = { 40, 112 };
static const int ROW_H = 64;
enum { ITEM_BROADCAST, ITEM_EXIT, ITEM_COUNT };
static const char* ITEM_TITLE[] = { "Broadcast a game", "Exit" };
static const char* ITEM_SUB[] = { "Arms Artic Base, then pick it on HOME", "Stops FTP" };

static C2D_TextBuf textBuf;
static C2D_TextBuf measureBuf;  // cleared per measurement so long paths never fill textBuf

static void text(float x, float y, float scale, u32 color, const char* str, u32 flags = 0) {
    C2D_Text t;
    C2D_TextParse(&t, textBuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
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
    text(MARGIN, 16, 0.55f, GREY, "Relay");
    snprintf(line, sizeof(line), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    text(TOP_W - MARGIN, 16, 0.5f, DIM, line, C2D_AlignRight);

    u32 mark = DIM;
    const char* label = "Starting";
    switch (s.state) {
        case State::Ready:     mark = LIVE;  label = "FTP ready"; break;
        case State::NoWifi:    mark = FAULT; label = "No Wi-Fi"; break;
        case State::NoSockets: mark = FAULT; label = "Network failed to start"; break;
        case State::Armed:     mark = LIVE;  label = "Broadcast armed"; break;
        case State::Starting:  break;
    }
    C2D_DrawRectSolid(MARGIN, 76, 0, 8, 8, mark);
    text(MARGIN + 16, 70, 0.55f, s.state == State::Ready || s.state == State::Armed ? WHITE : GREY, label);

    if (s.state == State::Armed) {
        text(MARGIN, 100, 0.95f, WHITE, "Pick a game on HOME");
        text(MARGIN, 152, 0.5f, GREY, "FTP returns inside the game");
        return;
    }

    if (s.ip) {
        struct in_addr a = { .s_addr = s.ip };
        snprintf(line, sizeof(line), "%s:%d", inet_ntoa(a), FTP_PORT);
        text(MARGIN, 100, 1.1f, WHITE, line);
    } else {
        text(MARGIN, 100, 1.1f, DIM, "-");
    }

    if (s.fault) {
        text(MARGIN, 152, 0.55f, FAULT, s.fault);
        return;
    }
    if (s.ftp.sessions > 0) snprintf(line, sizeof(line), "%d connected", s.ftp.sessions);
    else snprintf(line, sizeof(line), "Idle");
    text(MARGIN, 152, 0.55f, GREY, line);

    if (s.ftp.last[0]) {
        fitTail(line, sizeof(line), s.ftp.last, 0.5f, TOP_W - 2 * MARGIN);
        text(MARGIN, 176, 0.5f, DIM, line);
    }
    if (s.ftp.rx || s.ftp.tx) {
        char rx[24], tx[24];
        formatBytes(rx, sizeof(rx), s.ftp.rx);
        formatBytes(tx, sizeof(tx), s.ftp.tx);
        snprintf(line, sizeof(line), "received %s    sent %s", rx, tx);
        text(MARGIN, 208, 0.45f, DIM, line);
    }
}

static void drawBottom(int selected, const float* light, bool locked) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        streak(ROW_Y[i], ROW_H, light[i]);
        bool on = i == selected && !locked;
        text(MARGIN, ROW_Y[i] + 12, 0.65f, on ? WHITE : GREY, ITEM_TITLE[i]);
        text(MARGIN, ROW_Y[i] + 38, 0.45f, on ? GREY : DIM, ITEM_SUB[i]);
    }
    // U+E000 is the A button glyph in the system font.
    text(MARGIN, 212, 0.45f, DIM, " Select      START Exit");
}

int main(int argc, char* argv[]) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(2048);
    measureBuf = C2D_TextBufNew(256);

    // HOME would suspend the app and drop every FTP socket; sleep on lid close would too.
    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);

    Screen s;
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

    int selected = ITEM_BROADCAST;
    float light[ITEM_COUNT] = { 1.0f, 0.0f };
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
                    if (t.py >= ROW_Y[i] && t.py < ROW_Y[i] + ROW_H) { selected = i; activate = i; }
            }
        }

        if (activate == ITEM_EXIT) quit = true;
        if (activate == ITEM_BROADCAST) {
            s.fault = armBroadcast();
            if (!s.fault) s.state = State::Armed;
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
        }

        u64 title = 0;
        u8 media = 0;
        site_req_t req = site_take(&title, &media);
        if (req == SITE_LAUNCH) {
            // On exit libctru jumps to the chainload title, with the plugin armed for it.
            s.fault = armBroadcast();
            ftp_log("arm: %s", s.fault ? s.fault : "ok");
            if (!s.fault) { aptSetChainloader(title, media); quit = true; }
        } else if (req == SITE_REBOOT || req == SITE_POWEROFF) {
            svcSleepThread(300000000LL);  // the 200 reply leaves before the network goes
            site_power(req);
        }

        // Network state once a second: Wi-Fi can drop and return while the app sits here.
        if (socOk && s.state != State::Armed && frame % 60 == 0) {
            s.ip = (u32)gethostid();
            s.state = s.ip ? State::Ready : State::NoWifi;
        }
        if (s.state != State::Armed) ftp_status(&s.ftp);

        // Selection light crossfades in place, about 150 ms.
        for (int i = 0; i < ITEM_COUNT; i++) {
            float target = (i == selected && s.state != State::Armed) ? 1.0f : 0.0f;
            light[i] += (target - light[i]) * 0.3f;
        }

        C2D_TextBufClear(textBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, BG);
        C2D_SceneBegin(top);
        drawTop(s);
        C2D_TargetClear(bottom, BG);
        C2D_SceneBegin(bottom);
        drawBottom(selected, light, s.state == State::Armed);
        C3D_FrameEnd(0);

        frame++;
        // Long enough to read the armed screen, then hand back to HOME.
        if (s.state == State::Armed && ++armedFrames > 120) break;
    }

    ftp_stop();
    if (socUp) socExit();
    free(socBuffer);

    C2D_TextBufDelete(measureBuf);
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
