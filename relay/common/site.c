/* SITE verbs and the CIA install sink, shared by the launcher app and the plugin.
 * Runs on FTP session threads: anything that must happen after the reply goes out, or on the
 * main thread, is left as a request for site_take. */
#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "build.h"
#include "ftp.h"
#include "site.h"
#include "core.h"

#define SHOT_DIR "sdmc:/3ds/relay"

static bool is_app;
static volatile bool light_off;
/* One request waits at a time. A verb claims the slot as SITE_HOLD, fills its arguments, then posts;
 * site_take copies the arguments out before it frees the slot, so neither side sees them half-written. */
#define SITE_HOLD ((site_req_t)-1)
static volatile site_req_t pending;
static u64 pending_title;
static u8 pending_media;
static char pending_text[2][256], taken_text[2][256];

const char *site_text(int i) { return taken_text[i]; }

site_req_t site_take(u64 *title, u8 *media) {
    site_req_t r = __atomic_load_n(&pending, __ATOMIC_ACQUIRE);
    if (r == SITE_IDLE || r == SITE_HOLD) return SITE_IDLE;
    *title = pending_title;
    *media = pending_media;
    memcpy(taken_text, pending_text, sizeof(taken_text));
    __atomic_store_n(&pending, SITE_IDLE, __ATOMIC_RELEASE);
    return r;
}

/* False with the reply in out while an earlier request still waits for the main loop; answer 450. */
static bool claim(char *out, size_t cap) {
    site_req_t idle = SITE_IDLE;
    if (__atomic_compare_exchange_n(&pending, &idle, SITE_HOLD, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    snprintf(out, cap, "Busy with an earlier request, try again");
    return false;
}

void site_power(site_req_t req) {
    if (R_FAILED(ptmSysmInit())) { ftp_log("ptm:sysm unavailable"); return; }
    Result r = req == SITE_REBOOT ? PTMSYSM_RebootAsync(0) : PTMSYSM_ShutdownAsync(0);
    ptmSysmExit();
    if (R_FAILED(r)) ftp_log("%s failed %08lX", req == SITE_REBOOT ? "reboot" : "poweroff", r);
}

/* ---- screenshots ---- */

static void put_le(u8 *p, u32 v, int n) { for (int i = 0; i < n; i++) p[i] = v >> (8 * i); }

/* Framebuffers are stored rotated: one 240-pixel column per screen x, bottom pixel first. */
static const char *shot_screen(const GSPGPU_CaptureInfoEntry *e, int w, const char *path) {
    u32 fmt = e->format & 7;
    int bpp = fmt == GSP_RGBA8_OES ? 4 : fmt == GSP_BGR8_OES ? 3 : 2;
    u32 pitch = e->framebuf_widthbytesize ? e->framebuf_widthbytesize : 240 * bpp;
    const u8 *fb = (const u8 *)e->framebuf0_vaddr;
    if (!fb) return "no framebuffer";
    GSPGPU_InvalidateDataCache(fb, pitch * w);

    FILE *f = fopen(path, "wb");
    if (!f) return strerror(errno);
    u8 hdr[54] = { 'B', 'M' };
    u32 row_bytes = w * 3, img = row_bytes * 240;   /* 400*3 and 320*3 are already 4-aligned */
    put_le(hdr + 2, 54 + img, 4);
    put_le(hdr + 10, 54, 4);
    put_le(hdr + 14, 40, 4);
    put_le(hdr + 18, w, 4);
    put_le(hdr + 22, 240, 4);       /* positive height: rows bottom-up */
    put_le(hdr + 26, 1, 2);
    put_le(hdr + 28, 24, 2);
    put_le(hdr + 34, img, 4);
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    u8 row[400 * 3];
    for (int y = 239; ok && y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const u8 *p = fb + x * pitch + (239 - y) * bpp;
            u8 r, g, b;
            if (fmt == GSP_RGBA8_OES) { r = p[3]; g = p[2]; b = p[1]; }
            else if (fmt == GSP_BGR8_OES) { r = p[2]; g = p[1]; b = p[0]; }
            else {
                u16 v = p[0] | p[1] << 8;
                if (fmt == GSP_RGB565_OES) { r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; }
                else if (fmt == GSP_RGB5_A1_OES) { r = (v >> 11) << 3; g = ((v >> 6) & 31) << 3; b = ((v >> 1) & 31) << 3; }
                else { r = (v >> 12) << 4; g = ((v >> 8) & 15) << 4; b = ((v >> 4) & 15) << 4; }
            }
            row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
        }
        ok = fwrite(row, 1, row_bytes, f) == row_bytes;
    }
    if (fclose(f) != 0) ok = false;
    return ok ? NULL : "write failed";
}

static int site_shot(char *out, size_t cap) {
    GSPGPU_CaptureInfo info;
    Result r = GSPGPU_ImportDisplayCaptureInfo(&info);
    if (R_FAILED(r)) { snprintf(out, cap, "Capture failed %08lX", r); return 550; }
    mkdir(SHOT_DIR, 0777);
    const char *top = shot_screen(&info.screencapture[0], 400, SHOT_DIR "/top.bmp");
    const char *bot = shot_screen(&info.screencapture[1], 320, SHOT_DIR "/bottom.bmp");
    snprintf(out, cap, "/3ds/relay/top.bmp %s\n/3ds/relay/bottom.bmp %s", top ? top : "ok", bot ? bot : "ok");
    ftp_log("shot");
    return top || bot ? 550 : 200;
}

/* ---- status ---- */

static int site_stat(char *out, size_t cap) {
    size_t n = snprintf(out, cap, "mode %s\nbuild v%d.%d.%d %s", is_app ? "relay" : "broadcast",
                        VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION, RELAY_BUILD);

    u8 level = 0, charging = 0;
    bool have_level = R_SUCCEEDED(mcuHwcInit());
    if (have_level) { have_level = R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level)); mcuHwcExit(); }
    if (R_SUCCEEDED(ptmuInit())) { PTMU_GetBatteryChargeState(&charging); ptmuExit(); }
    if (have_level) n += snprintf(out + n, cap - n, "\nbattery %u%%%s", level, charging ? " charging" : "");
    else n += snprintf(out + n, cap - n, "\nbattery unknown%s", charging ? " charging" : "");

    n += snprintf(out + n, cap - n, "\nwifi %u/3", osGetWifiStrength());

    FS_ArchiveResource res;
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD))) {
        double unit = (double)res.clusterSize / 1e9;
        n += snprintf(out + n, cap - n, "\nsd free %.1f GB of %.1f GB", res.freeClusters * unit, res.totalClusters * unit);
    }
    u64 tid = 0;
    if (R_SUCCEEDED(APT_GetProgramID(&tid)))
        n += snprintf(out + n, cap - n, "\ntitle %016llX", tid);
    n += snprintf(out + n, cap - n, "\nlight %s", light_off ? "off" : "on");
    if (is_app) snprintf(out + n, cap - n, "\ncore %s", core_running() ? "running" : "absent");
    return 200;
}

bool site_set_light(bool off) {
    if (R_FAILED(gspLcdInit())) return false;
    Result r = off ? GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH) : GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
    if (R_FAILED(r)) { ftp_log("backlight %s failed %08lX", off ? "off" : "on", r); return false; }
    light_off = off;
    return true;
}

bool site_light_is_off(void) { return light_off; }

static int site_light(const char *arg, char *out, size_t cap) {
    bool off = !strcasecmp(arg, "off");
    if (!off && strcasecmp(arg, "on")) { snprintf(out, cap, "SITE LIGHT on|off"); return 501; }
    if (!site_set_light(off)) { snprintf(out, cap, "Backlight failed, see SITE LOG"); return 550; }
    snprintf(out, cap, "Backlight %s", off ? "off" : "on");
    return 200;
}

/* ---- titles ---- */

/* Lists application titles on one medium; returns bytes written. */
static size_t list_titles(FS_MediaType media, const char *where, char *out, size_t cap) {
    u32 count = 0, read = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || !count) return 0;
    u64 *ids = malloc(count * sizeof(u64));
    if (!ids) return 0;
    size_t n = 0;
    if (R_SUCCEEDED(AM_GetTitleList(&read, media, count, ids))) {
        for (u32 i = 0; i < read && n < cap - 64; i++) {
            if ((ids[i] >> 32) != 0x00040000) continue;   /* applications only */
            char code[17] = { 0 };   /* AM fills 16 with no terminator */
            AM_GetTitleProductCode(media, ids[i], code);
            n += snprintf(out + n, cap - n, "%s%016llX %s %s", n ? "\n" : "", ids[i], where, code);
        }
    }
    free(ids);
    return n;
}

static int site_titles(char *out, size_t cap) {
    if (R_FAILED(amInit())) { snprintf(out, cap, "AM unavailable"); return 550; }
    size_t n = list_titles(MEDIATYPE_SD, "sd", out, cap);
    if (n) { out[n++] = '\n'; out[n] = '\0'; }
    size_t card = list_titles(MEDIATYPE_GAME_CARD, "card", out + n, cap - n);
    if (card) n += card;
    else if (n) out[--n] = '\0';   /* no card: drop the separator */
    amExit();
    if (!n) snprintf(out, cap, "No titles");
    return 200;
}

/* Finds the medium holding tid; false if neither SD nor the card has it. */
static bool title_media(u64 tid, u8 *media) {
    static const FS_MediaType order[] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
    bool found = false;
    if (R_FAILED(amInit())) return false;
    for (int m = 0; m < 2 && !found; m++) {
        AM_TitleInfo info;
        if (R_SUCCEEDED(AM_GetTitleInfo(order[m], 1, &tid, &info))) { *media = order[m]; found = true; }
    }
    amExit();
    return found;
}

static int site_launch(const char *arg, char *out, size_t cap) {
    if (!is_app) { snprintf(out, cap, "In a game; SITE PLAY boots it, LAUNCH needs the Relay app"); return 550; }
    char *end;
    u64 tid = strtoull(arg, &end, 16);
    if (!*arg || *end) { snprintf(out, cap, "SITE LAUNCH <16 hex digit title id>; SITE TITLES lists them"); return 501; }
    u8 media;
    if (!title_media(tid, &media)) { snprintf(out, cap, "%016llX not installed", tid); return 550; }
    if (!claim(out, cap)) return 450;
    pending_title = tid;
    pending_media = media;
    __atomic_store_n(&pending, SITE_LAUNCH, __ATOMIC_RELEASE);
    ftp_log("launch %016llX", tid);
    u64 self = 0;
    APT_GetProgramID(&self);
    if (tid == self) snprintf(out, cap, "Relaunching Relay; FTP returns as \"trey relay\"");
    else snprintf(out, cap, "Arming broadcast, then starting %016llX; FTP returns as \"trey broadcast\"", tid);
    return 200;
}

/* The main loop uninstalls: it holds the kept list and the SMDH names. */
static int site_uninstall(const char *arg, char *out, size_t cap) {
    if (!is_app) { snprintf(out, cap, "In a game; needs the Relay app"); return 550; }
    char *end;
    u64 tid = strtoull(arg, &end, 16);
    if (!*arg || *end) { snprintf(out, cap, "SITE UNINSTALL <16 hex digit title id>; SITE TITLES lists them"); return 501; }
    if (!claim(out, cap)) return 450;
    pending_title = tid;
    __atomic_store_n(&pending, SITE_UNINSTALL, __ATOMIC_RELEASE);
    snprintf(out, cap, "Started; SITE LOG shows the result");
    return 200;
}

/* %XX escapes decoded in place, so names and passwords can carry spaces. A % without two hex
 * digits stays as it is. */
static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtoul(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = '\0';
}

static int site_fetch(const char *arg, char *out, size_t cap) {
    if (!is_app) { snprintf(out, cap, "In a game; needs the Relay app"); return 550; }
    if (strncasecmp(arg, "http://", 7) && strncasecmp(arg, "https://", 8)) { snprintf(out, cap, "SITE FETCH <http(s) url of a CIA>"); return 501; }
    if (strlen(arg) >= sizeof(pending_text[0])) { snprintf(out, cap, "URL too long"); return 501; }
    if (!claim(out, cap)) return 450;
    snprintf(pending_text[0], sizeof(pending_text[0]), "%s", arg);
    __atomic_store_n(&pending, SITE_FETCH, __ATOMIC_RELEASE);
    snprintf(out, cap, "Started; SITE LOG shows the result");
    return 200;
}

/* SITE WIFI <1-3> <name> [password], each %XX-escaped. Saving cycles Wi-Fi: FTP drops for about 10 s. */
static int site_wifi(char *arg, char *out, size_t cap) {
    if (!is_app) { snprintf(out, cap, "In a game; needs the Relay app"); return 550; }
    char *name = strchr(arg, ' ');
    if (name) while (*name == ' ') *name++ = '\0';
    char *pass = name ? strchr(name, ' ') : NULL;
    if (pass) while (*pass == ' ') *pass++ = '\0';
    int slot = atoi(arg);
    if (slot < 1 || slot > 3 || !name || !*name || strlen(name) > 96 || (pass && strlen(pass) > 192)) {
        snprintf(out, cap, "SITE WIFI <1-3> <name> [password], %%XX-escaped, no password = open network");
        return 501;
    }
    if (!claim(out, cap)) return 450;
    snprintf(pending_text[0], sizeof(pending_text[0]), "%s", name);
    snprintf(pending_text[1], sizeof(pending_text[1]), "%s", pass ? pass : "");
    unescape(pending_text[0]);
    unescape(pending_text[1]);
    pending_title = slot - 1;
    __atomic_store_n(&pending, SITE_WIFI, __ATOMIC_RELEASE);
    snprintf(out, cap, "Saving connection %d, then connecting; FTP drops for about 10 s, SITE LOG shows the result", slot);
    return 200;
}

static int request(site_req_t r, const char *what, char *out, size_t cap) {
    if (!claim(out, cap)) return 450;
    __atomic_store_n(&pending, r, __ATOMIC_RELEASE);
    ftp_log("%s", what);
    snprintf(out, cap, "%s; FTP goes down", what);
    return 200;
}

static const char HELP[] =
    "SITE STAT        mode, build, battery, wifi, sd free, title, light\n"
    "SITE SHOT        both screens to /3ds/relay/top.bmp, bottom.bmp\n"
    "SITE LOG         recent events\n"
    "SITE LIGHT on|off  backlight, both screens\n"
    "SITE TITLES      installed applications: id medium product code\n"
    "SITE LAUNCH <id> Relay app only: arm broadcast, start the title\n"
    "SITE UNINSTALL <id>  Relay app only: SD application, relay FBI 3hs refused, result in SITE LOG\n"
    "SITE FETCH <url> Relay app only: downloads and installs a CIA, result in SITE LOG\n"
    "SITE WIFI <1-3> <name> [password]  Relay app only: %XX-escaped, saves the connection and connects\n"
    "SITE PLAY        plugin only: leave the plugin screen, boot the game\n"
    "SITE SYNC | SITE INBOX | SITE UPDATE  Relay app only: the menu rows, result in SITE LOG\n"
    "SITE CLOCK       Relay app only: set the clock from NTP, result in SITE LOG\n"
    "SITE REBOOT | SITE POWEROFF\n"
    "STOR /install/<name>.cia  Relay app only: installs, nothing written to the card";

static int site(char *arg, char *out, size_t cap) {
    char *rest = strchr(arg, ' ');
    if (rest) { *rest++ = '\0'; while (*rest == ' ') rest++; } else rest = arg + strlen(arg);

    if (!strcasecmp(arg, "HELP")) { snprintf(out, cap, "%s", HELP); return 214; }
    if (!strcasecmp(arg, "STAT")) return site_stat(out, cap);
    if (!strcasecmp(arg, "SHOT")) return site_shot(out, cap);
    if (!strcasecmp(arg, "LIGHT")) return site_light(rest, out, cap);
    if (!strcasecmp(arg, "TITLES")) return site_titles(out, cap);
    if (!strcasecmp(arg, "LAUNCH")) return site_launch(rest, out, cap);
    if (!strcasecmp(arg, "FETCH")) return site_fetch(rest, out, cap);
    if (!strcasecmp(arg, "WIFI")) return site_wifi(rest, out, cap);
    if (!strcasecmp(arg, "UNINSTALL")) return site_uninstall(rest, out, cap);
    if (!strcasecmp(arg, "REBOOT")) return request(SITE_REBOOT, "Rebooting", out, cap);
    if (!strcasecmp(arg, "POWEROFF")) return request(SITE_POWEROFF, "Powering off", out, cap);
    if (!strcasecmp(arg, "SYNC") || !strcasecmp(arg, "INBOX") || !strcasecmp(arg, "UPDATE") || !strcasecmp(arg, "CLOCK")) {
        if (!is_app) { snprintf(out, cap, "In a game; needs the Relay app"); return 550; }
        site_req_t r = !strcasecmp(arg, "SYNC") ? SITE_SYNC : !strcasecmp(arg, "INBOX") ? SITE_INBOX
                     : !strcasecmp(arg, "CLOCK") ? SITE_CLOCK : SITE_UPDATE;
        if (!claim(out, cap)) return 450;
        __atomic_store_n(&pending, r, __ATOMIC_RELEASE);
        snprintf(out, cap, "Started; SITE LOG shows the result");
        return 200;
    }
    if (!strcasecmp(arg, "PLAY")) {
        if (is_app) { snprintf(out, cap, "Not in a game; SITE LAUNCH <id>"); return 550; }
        return request(SITE_PLAY, "Booting the game", out, cap);
    }
    return 0;
}

/* ---- CIA install sink ---- */

typedef struct { Handle h; u64 off; } cia_t;
static volatile bool installing;   /* one at a time: FTP sessions and the main loop both install */

void *cia_begin(const char **err) {
    if (__atomic_exchange_n(&installing, true, __ATOMIC_ACQ_REL)) { *err = "Another install is running"; return NULL; }
    if (R_FAILED(amInit())) { installing = false; *err = "AM unavailable"; return NULL; }
    cia_t *c = calloc(1, sizeof(*c));
    Result r = c ? AM_StartCiaInstall(MEDIATYPE_SD, &c->h) : -1;
    if (R_FAILED(r)) {
        free(c);
        amExit();
        installing = false;
        ftp_log("AM_StartCiaInstall %08lX", r);
        *err = "Installer refused to start";
        return NULL;
    }
    return c;
}

static int sink_open(const char *abs, void **ctx, const char **err) {
    size_t n = strlen(abs);
    if (strncmp(abs, "/install/", 9) || n < 13 || strcasecmp(abs + n - 4, ".cia")) return 0;
    if (!is_app) { *err = "Install from the Relay app, not in a game"; return -1; }
    return (*ctx = cia_begin(err)) ? 1 : -1;
}

int cia_write(void *ctx, const void *p, size_t n) {
    cia_t *c = ctx;
    u32 written = 0;
    Result r = FSFILE_Write(c->h, &written, c->off, p, n, 0);
    c->off += written;
    if (R_FAILED(r)) ftp_log("cia write %08lX at %llu", r, c->off);
    return R_SUCCEEDED(r) && written == n;
}

/* AM takes the handle on finish and on cancel. */
const char *cia_end(void *ctx, int ok) {
    static char why[48];
    cia_t *c = ctx;
    Result r = ok ? AM_FinishCiaInstall(c->h) : AM_CancelCIAInstall(c->h);
    free(c);
    amExit();
    installing = false;
    if (!ok) return "Install cancelled";
    if (R_FAILED(r)) {
        snprintf(why, sizeof(why), "Install failed %08lX", r);
        return why;
    }
    return NULL;
}

static const ftp_hooks_t hooks = { site, sink_open, cia_write, cia_end };

void site_init(bool app) {
    is_app = app;
    ftp_set_hooks(&hooks);
}
