#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <string>
#include <vector>
#include <algorithm>

#include "ftp.h"
#include "saves.hpp"

// Two-way sync between each title's save and extdata and their packed copies on the card,
// sdmc:/3ds/relay/saves/<TID>.sav and <TID>.ext, which a computer reads and writes over FTP (`trey saves`).
// sdmc:/3ds/relay/sync/<TID> (save) and <TID>.ext hold the hash both sides had after the last sync, so
// each side's change since then is known: one side changed = that side wins; both changed, or never
// synced = the console wins. Whatever gets replaced is kept first as saves/old/<TID>/<stamp>.sav|.ext,
// the newest KEEP per kind. saves/in/<TID>.sav|.ext always wins: restored over the console's copy, then
// moved to saves/. A Saves page restore goes through in/ the same way.
// Pack form: per file u16 path length, path, u32 size, data, all little-endian, sorted by path; u16 0 ends.
// Hash: 64-bit FNV-1a of the whole pack.
// Checkpoint: its backups in /3ds/Checkpoint/{saves,extdata}/<0xUNIQUEID name>/<folder>/ (raw files) are
// listed for restore; with sdmc:/3ds/relay/checkpoint present every relay backup is written there too.

static const char* STATE_DIR = "sdmc:/3ds/relay/sync";
static const char* STORE_DIR = "sdmc:/3ds/relay/saves";
static const char* CP_FLAG = "sdmc:/3ds/relay/checkpoint";
enum Kind { SAVE, EXT, KINDS };
static const char* SUF[KINDS] = { "sav", "ext" };
static const char* KIND_NAME[KINDS] = { "save", "extdata" };
static const char* CP_DIR[KINDS] = { "sdmc:/3ds/Checkpoint/saves", "sdmc:/3ds/Checkpoint/extdata" };
static const int KEEP = 10;
// Whole archive is held in memory up to three times (files, pack, card copy); Old 3DS apps get ~64 MB.
static const size_t MAX_BYTES = 8u << 20;

struct SaveFile { std::string path; std::vector<u8> data; };

static u64 fnv(const std::vector<u8>& b) {
    u64 h = 0xcbf29ce484222325ULL;
    for (u8 c : b) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

static FS_Path utf16Path(const std::string& p, std::vector<u16>& buf) {
    buf.assign(p.size() + 1, 0);
    ssize_t n = utf8_to_utf16(buf.data(), (const u8*)p.c_str(), p.size());
    buf.resize(n < 0 ? 1 : n + 1);
    buf.back() = 0;
    return (FS_Path){ PATH_UTF16, buf.size() * 2, buf.data() };
}


// Extdata id is the unique id for nearly every title. TODO(later): a few share or move theirs (Checkpoint
// keeps an override list); those sync nothing until an override table is added here.
static u32 extId(u64 tid) { return (u32)tid >> 8; }

static bool openArchive(u64 tid, FS_MediaType media, Kind k, FS_Archive* arch) {
    if (k == SAVE) {
        u32 path[3] = { media, (u32)tid, (u32)(tid >> 32) };
        return R_SUCCEEDED(FSUSER_OpenArchive(arch, ARCHIVE_USER_SAVEDATA, (FS_Path){ PATH_BINARY, 12, path }));
    }
    u32 path[3] = { MEDIATYPE_SD, extId(tid), 0 };
    return R_SUCCEEDED(FSUSER_OpenArchive(arch, ARCHIVE_EXTDATA, (FS_Path){ PATH_BINARY, 12, path }));
}

// dir is "" for the root or "a/b"; files come out as "a/b/name". False past MAX_BYTES, total says so.
static bool readTree(FS_Archive arch, const std::string& dir, std::vector<SaveFile>& out, size_t& total) {
    std::vector<u16> pb;
    Handle d;
    if (R_FAILED(FSUSER_OpenDirectory(&d, arch, utf16Path("/" + dir, pb)))) return false;
    bool ok = true;
    for (;;) {
        FS_DirectoryEntry e;
        u32 got = 0;
        if (R_FAILED(FSDIR_Read(d, &got, 1, &e))) { ok = false; break; }
        if (!got) break;
        char name[0x106 * 3] = { 0 };
        utf16_to_utf8((u8*)name, e.name, sizeof(name) - 1);
        std::string rel = dir.empty() ? name : dir + "/" + name;
        if (e.attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (!readTree(arch, rel, out, total)) { ok = false; break; }
            continue;
        }
        Handle f;
        if (R_FAILED(FSUSER_OpenFile(&f, arch, utf16Path("/" + rel, pb), FS_OPEN_READ, 0))) { ok = false; break; }
        u64 size = 0;
        FSFILE_GetSize(f, &size);
        if ((total += size) > MAX_BYTES) { FSFILE_Close(f); ok = false; break; }
        SaveFile sf{ rel, std::vector<u8>(size) };
        u32 rd = 0;
        Result r = size ? FSFILE_Read(f, &rd, 0, sf.data.data(), size) : 0;
        FSFILE_Close(f);
        if (R_FAILED(r) || rd != size) { ok = false; break; }
        out.push_back(std::move(sf));
    }
    FSDIR_Close(d);
    return ok;
}

static std::vector<u8> pack(std::vector<SaveFile>& files) {
    std::sort(files.begin(), files.end(), [](const SaveFile& a, const SaveFile& b) { return a.path < b.path; });
    std::vector<u8> out;
    auto put = [&](u32 v, int n) { for (int i = 0; i < n; i++) out.push_back(v >> (8 * i)); };
    for (auto& f : files) {
        put(f.path.size(), 2);
        out.insert(out.end(), f.path.begin(), f.path.end());
        put(f.data.size(), 4);
        out.insert(out.end(), f.data.begin(), f.data.end());
    }
    put(0, 2);
    return out;
}

static bool unpack(const u8* b, u32 len, std::vector<SaveFile>& out) {
    u32 i = 0;
    auto get = [&](int n, u32* v) {
        if (i + n > len) return false;
        *v = 0;
        for (int k = 0; k < n; k++) *v |= (u32)b[i++] << (8 * k);
        return true;
    };
    for (;;) {
        u32 n, size;
        if (!get(2, &n)) return false;
        if (!n) return i == len;
        if (i + n > len) return false;
        std::string path((const char*)b + i, n);
        i += n;
        if (!get(4, &size) || i + size > len || path[0] == '/' || path.find("..") != std::string::npos) return false;
        out.push_back({ path, std::vector<u8>(b + i, b + i + size) });
        i += size;
    }
}

// Save: the archive's contents are replaced, then committed; secure value cleared for SD titles so the
// game takes a save it did not write itself. Extdata: each file deleted and recreated at its new size
// (extdata files cannot grow), others left, no commit.
static bool writeArchive(u64 tid, FS_MediaType media, Kind k, const std::vector<SaveFile>& files) {
    FS_Archive arch;
    if (!openArchive(tid, media, k, &arch)) return false;
    std::vector<u16> pb;
    bool ok = true;
    Handle d;
    if (k == SAVE && R_SUCCEEDED(FSUSER_OpenDirectory(&d, arch, utf16Path("/", pb)))) {
        std::vector<std::pair<std::string, bool>> top;
        for (;;) {
            FS_DirectoryEntry e;
            u32 got = 0;
            if (R_FAILED(FSDIR_Read(d, &got, 1, &e)) || !got) break;
            char name[0x106 * 3] = { 0 };
            utf16_to_utf8((u8*)name, e.name, sizeof(name) - 1);
            top.push_back({ std::string("/") + name, (e.attributes & FS_ATTRIBUTE_DIRECTORY) != 0 });
        }
        FSDIR_Close(d);
        for (auto& t : top) {
            Result r = t.second ? FSUSER_DeleteDirectoryRecursively(arch, utf16Path(t.first, pb))
                                : FSUSER_DeleteFile(arch, utf16Path(t.first, pb));
            if (R_FAILED(r)) ok = false;
        }
    }
    for (size_t i = 0; ok && i < files.size(); i++) {
        const std::string& p = files[i].path;
        for (size_t s = p.find('/'); s != std::string::npos; s = p.find('/', s + 1))
            FSUSER_CreateDirectory(arch, utf16Path("/" + p.substr(0, s), pb), 0);   // exists is fine
        FS_Path fp = utf16Path("/" + p, pb);
        if (k == EXT) FSUSER_DeleteFile(arch, fp);   // missing is fine
        Handle f;
        if (R_FAILED(FSUSER_CreateFile(arch, fp, 0, files[i].data.size())) ||
            R_FAILED(FSUSER_OpenFile(&f, arch, fp, FS_OPEN_WRITE, 0))) { ok = false; break; }
        u32 w = 0;
        Result r = files[i].data.empty() ? 0
                 : FSFILE_Write(f, &w, 0, files[i].data.data(), files[i].data.size(), FS_WRITE_FLUSH);
        FSFILE_Close(f);
        if (R_FAILED(r) || w != files[i].data.size()) ok = false;
    }
    if (ok && k == SAVE) ok = R_SUCCEEDED(FSUSER_ControlArchive(arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA, nullptr, 0, nullptr, 0));
    FSUSER_CloseArchive(arch);
    if (ok && k == SAVE && media == MEDIATYPE_SD) {
        u64 in = ((u64)SECUREVALUE_SLOT_SD << 32) | (((u32)tid >> 8) & 0xFFFFF) << 8;
        u8 out;
        FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &in, 8, &out, 1);
    }
    return ok;
}

static std::string statePath(u64 tid, Kind k) {
    char p[64];
    snprintf(p, sizeof(p), "%s/%016llX%s", STATE_DIR, tid, k == SAVE ? "" : ".ext");
    return p;
}

// sub is "" for the store itself or "in/".
static std::string storePath(u64 tid, Kind k, const char* sub = "") {
    char p[80];
    snprintf(p, sizeof(p), "%s/%s%016llX.%s", STORE_DIR, sub, tid, SUF[k]);
    return p;
}

static u64 lastHash(u64 tid, Kind k) {
    FILE* f = fopen(statePath(tid, k).c_str(), "r");
    unsigned long long h = 0;
    if (f) { if (fscanf(f, "%llx", &h) != 1) h = 0; fclose(f); }
    return h;
}

static void setLast(u64 tid, Kind k, u64 h) {
    mkdir("sdmc:/3ds/relay", 0777);
    mkdir(STATE_DIR, 0777);
    FILE* f = fopen(statePath(tid, k).c_str(), "w");
    if (f) { fprintf(f, "%016llx\n", h); fclose(f); }
}

static bool readFile(const char* path, std::vector<u8>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n < 0 ? 0 : n);
    bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Written beside the target, then renamed over it: an FTP client never sees half a file.
static bool writeFile(const char* path, const std::vector<u8>& b) {
    std::string tmp = std::string(path) + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (!ok || rename(tmp.c_str(), path)) { remove(tmp.c_str()); return false; }
    return true;
}

static void mkdirs(const std::string& path) {
    for (size_t s = path.find('/', 6); s != std::string::npos; s = path.find('/', s + 1))
        mkdir(path.substr(0, s).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// Same form as Checkpoint's default backup folder name.
static void stamp(char* out, size_t cap, time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);   // the 3DS clock is local time with no zone
    strftime(out, cap, "%Y%m%d-%H%M%S", &tm);
}

static std::vector<std::string> listDir(const std::string& dir, bool dirs) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        bool isDir = e->d_type == DT_DIR;   // libsysbase fills it from the FS directory attribute
        struct stat st;
        if (e->d_type == DT_UNKNOWN) {
            if (stat((dir + "/" + e->d_name).c_str(), &st)) continue;
            isDir = S_ISDIR(st.st_mode);
        }
        if (isDir == dirs) out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// SMDH short name in English, Japanese when that is empty; the title id when the icon is unreadable.
static void titleName(u64 tid, u8 media, char* out, size_t cap) {
    snprintf(out, cap, "%016llX", tid);
    static const u32 file[5] = { 0, 0, 2, 0x6E6F6369, 0 };   // exefs "icon"
    u32 arch[4] = { (u32)tid, (u32)(tid >> 32), media, 0 };
    Handle h;
    if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, (FS_Path){ PATH_BINARY, 16, arch },
                                         (FS_Path){ PATH_BINARY, 20, file }, FS_OPEN_READ, 0))) return;
    static u8 smdh[0x36C0];   // whole SMDH: the exefs file refuses a shorter read
    u32 rd = 0;
    Result r = FSFILE_Read(h, &rd, 0, smdh, sizeof(smdh));
    FSFILE_Close(h);
    if (R_FAILED(r) || rd != sizeof(smdh) || memcmp(smdh, "SMDH", 4)) return;
    for (int lang : { 1, 0 }) {
        u16 name[0x41] = { 0 };
        memcpy(name, smdh + 8 + lang * 0x200, 0x80);
        if (!name[0]) continue;
        char u[0x41 * 3] = { 0 };
        utf16_to_utf8((u8*)u, name, sizeof(u) - 1);
        for (char* c = u; *c; c++) if (*c == '\n') *c = ' ';
        snprintf(out, cap, "%s", u);
        return;
    }
}

// Checkpoint's title folder: "0x<unique id> <name>". Found by its number, so a renamed title still matches.
static std::string cpTitleDir(u64 tid, u8 media, Kind k, bool create) {
    for (auto& n : listDir(CP_DIR[k], true))
        if (!strncasecmp(n.c_str(), "0x", 2) && strtoul(n.c_str() + 2, nullptr, 16) == extId(tid))
            return std::string(CP_DIR[k]) + "/" + n;
    if (!create) return "";
    char name[64], dir[96];
    titleName(tid, media, name, sizeof(name));
    for (char* c = name; *c; c++) if (strchr(".,!\\/:?*\"<>|", *c)) *c = ' ';
    snprintf(dir, sizeof(dir), "0x%05lX %s", (unsigned long)extId(tid), name);
    return std::string(CP_DIR[k]) + "/" + dir;
}

static bool writeRawTree(const std::string& dir, const std::vector<SaveFile>& files) {
    for (auto& f : files) {
        std::string p = dir + "/" + f.path;
        mkdirs(p.substr(0, p.rfind('/')));
        FILE* o = fopen(p.c_str(), "wb");
        if (!o) return false;
        bool ok = fwrite(f.data.data(), 1, f.data.size(), o) == f.data.size();
        if (fclose(o) || !ok) return false;
    }
    return true;
}

static bool readRawTree(const std::string& root, const std::string& rel, std::vector<SaveFile>& out, size_t& total) {
    std::string dir = rel.empty() ? root : root + "/" + rel;
    for (auto& n : listDir(dir, true)) if (!readRawTree(root, rel.empty() ? n : rel + "/" + n, out, total)) return false;
    for (auto& n : listDir(dir, false)) {
        SaveFile f{ rel.empty() ? n : rel + "/" + n, {} };
        if (!readFile((root + "/" + f.path).c_str(), f.data) || (total += f.data.size()) > MAX_BYTES) return false;
        out.push_back(std::move(f));
    }
    return true;
}

// Keeps b as the newest backup of tid's k, drops the oldest past KEEP.
static bool backup(u64 tid, FS_MediaType media, Kind k, const std::vector<u8>& b) {
    char dir[64], st[20];
    snprintf(dir, sizeof(dir), "%s/old/%016llX", STORE_DIR, tid);
    mkdirs(dir);
    // One-copy form saves/old/<TID>.sav moves in under its own time.
    char legacyp[64];
    snprintf(legacyp, sizeof(legacyp), "%s/old/%016llX.sav", STORE_DIR, tid);
    std::string legacy = legacyp;
    u64 mt = 0;
    if (k == SAVE && exists(legacy)) {
        stamp(st, sizeof(st), archive_getmtime(legacy.c_str(), &mt) == 0 ? (time_t)mt : 0);
        rename(legacy.c_str(), (std::string(dir) + "/" + st + ".sav").c_str());
    }
    stamp(st, sizeof(st), time(NULL));
    std::string p = std::string(dir) + "/" + st + "." + SUF[k];
    if (!writeFile(p.c_str(), b)) return false;
    std::vector<std::string> mine;
    for (auto& n : listDir(dir, false))
        if (n.size() > 4 && !strcmp(n.c_str() + n.size() - 4, (std::string(".") + SUF[k]).c_str())) mine.push_back(n);
    // Stamps order backups only while the clock is set; a reset clock (2011) would prune the newest.
    if (time(NULL) < 1577836800) ftp_log("clock before 2020, set it in System Settings; backups kept unpruned");
    else for (int i = 0; i + KEEP < (int)mine.size(); i++) remove((std::string(dir) + "/" + mine[i]).c_str());

    if (exists(CP_FLAG)) {
        std::vector<SaveFile> files;
        std::string cp = cpTitleDir(tid, media, k, true) + "/" + st;
        if (!unpack(b.data(), b.size(), files) || !writeRawTree(cp, files)) ftp_log("save %016llX: Checkpoint copy failed", tid);
    }
    return true;
}

enum class Outcome { Same, Pushed, Pulled, NoSave, Failed };

static Outcome syncOne(u64 tid, FS_MediaType media, Kind k) {
    FS_Archive arch;
    if (!openArchive(tid, media, k, &arch)) return Outcome::NoSave;
    std::vector<SaveFile> files;
    size_t total = 0;
    bool read = readTree(arch, "", files, total);
    FSUSER_CloseArchive(arch);
    if (total > MAX_BYTES) { ftp_log("%s %016llX over %u MB, skipped", KIND_NAME[k], tid, MAX_BYTES >> 20); return Outcome::NoSave; }
    if (!read) { ftp_log("%s %016llX unreadable", KIND_NAME[k], tid); return Outcome::Failed; }
    if (files.empty()) return Outcome::NoSave;
    std::vector<u8> local = pack(files);
    files = {};
    u64 lh = fnv(local), last = lastHash(tid, k);
    const char* kn = KIND_NAME[k];

    std::string store = storePath(tid, k), forced = storePath(tid, k, "in/");
    std::vector<u8> in;
    if (readFile(forced.c_str(), in)) {
        u64 ph = fnv(in);
        if (ph != lh) {
            std::vector<SaveFile> unpacked;
            if (!unpack(in.data(), in.size(), unpacked)) { ftp_log("%s %016llX: in/ copy unreadable", kn, tid); return Outcome::Failed; }
            if (!backup(tid, media, k, local)) { ftp_log("%s %016llX: backup failed", kn, tid); return Outcome::Failed; }
            if (!writeArchive(tid, media, k, unpacked)) { ftp_log("%s %016llX: restore failed", kn, tid); return Outcome::Failed; }
        }
        if (!writeFile(store.c_str(), in)) { ftp_log("%s %016llX: write failed", kn, tid); return Outcome::Failed; }
        remove(forced.c_str());
        setLast(tid, k, ph);
        ftp_log("%s %016llX restored from in/", kn, tid);
        return ph == lh ? Outcome::Same : Outcome::Pulled;
    }
    std::vector<u8> remote;
    bool have = readFile(store.c_str(), remote);
    u64 rh = have ? fnv(remote) : 0;

    if (have && rh == lh) { if (last != lh) setLast(tid, k, lh); return Outcome::Same; }
    bool pull = have && lh == last && rh != last;   // only the card copy moved
    if (!pull) {
        if (have && rh != last && !backup(tid, media, k, remote)) { ftp_log("%s %016llX: backup failed", kn, tid); return Outcome::Failed; }
        if (!writeFile(store.c_str(), local)) { ftp_log("%s %016llX: write failed", kn, tid); return Outcome::Failed; }
        setLast(tid, k, lh);
        ftp_log("%s %016llX sent", kn, tid);
        return Outcome::Pushed;
    }
    std::vector<SaveFile> unpacked;
    if (!backup(tid, media, k, local)) { ftp_log("%s %016llX: backup failed", kn, tid); return Outcome::Failed; }
    if (!unpack(remote.data(), remote.size(), unpacked) || !writeArchive(tid, media, k, unpacked)) {
        ftp_log("%s %016llX: restore failed", kn, tid);
        return Outcome::Failed;
    }
    setLast(tid, k, rh);
    ftp_log("%s %016llX restored", kn, tid);
    return Outcome::Pulled;
}

// Applications on SD and card, with their media.
static std::vector<std::pair<u64, FS_MediaType>> apps() {
    std::vector<std::pair<u64, FS_MediaType>> out;
    static const FS_MediaType media[] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
    for (FS_MediaType m : media) {
        u32 count = 0, got = 0;
        if (R_FAILED(AM_GetTitleCount(m, &count)) || !count) continue;
        std::vector<u64> ids(count);
        if (R_FAILED(AM_GetTitleList(&got, m, count, ids.data()))) continue;
        for (u32 i = 0; i < got; i++) if ((ids[i] >> 32) == 0x00040000) out.push_back({ ids[i], m });
    }
    return out;
}

bool syncSaves(char* msg, size_t cap) {
    if (R_FAILED(amInit())) { snprintf(msg, cap, "AM unavailable"); return false; }
    mkdirs(std::string(STORE_DIR) + "/old");
    mkdirs(std::string(STORE_DIR) + "/in");
    int pushed = 0, pulled = 0, same = 0, failed = 0;
    for (auto& [tid, m] : apps()) {
        for (int k = 0; k < KINDS; k++) {
            switch (syncOne(tid, m, (Kind)k)) {
                case Outcome::Pushed: pushed++; break;
                case Outcome::Pulled: pulled++; break;
                case Outcome::Same:   same++; break;
                case Outcome::Failed: failed++; break;
                case Outcome::NoSave: break;
            }
        }
    }
    amExit();
    snprintf(msg, cap, "%d saved, %d restored, %d same%s", pushed, pulled, same,
             failed ? ", some failed, see log" : "");
    ftp_log("saves: %s", msg);
    return !failed;
}

std::vector<SaveTitle> saveTitles() {
    std::vector<SaveTitle> out;
    if (R_FAILED(amInit())) return out;
    for (auto& [tid, m] : apps()) {
        bool sav = exists(storePath(tid, SAVE)), ext = exists(storePath(tid, EXT));
        bool cp = !cpTitleDir(tid, m, SAVE, false).empty() || !cpTitleDir(tid, m, EXT, false).empty();
        if (!sav && !ext && !cp) continue;
        SaveTitle t{ tid, (u8)m, "", 0, ext };
        titleName(tid, m, t.name, sizeof(t.name));
        u64 mt = 0;
        if (archive_getmtime(storePath(tid, sav ? SAVE : EXT).c_str(), &mt) == 0) t.synced = (time_t)mt;
        out.push_back(t);
    }
    amExit();
    std::sort(out.begin(), out.end(), [](const SaveTitle& a, const SaveTitle& b) { return strcasecmp(a.name, b.name) < 0; });
    return out;
}

std::vector<SaveBackup> saveBackups(const SaveTitle& t) {
    std::vector<std::pair<std::string, SaveBackup>> all;   // sort key: the stamp or folder name
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/old/%016llX", STORE_DIR, t.tid);
    for (auto& n : listDir(dir, false)) {
        int k = n.size() > 4 && !strcmp(n.c_str() + n.size() - 4, ".ext") ? EXT
              : n.size() > 4 && !strcmp(n.c_str() + n.size() - 4, ".sav") ? SAVE : -1;
        if (k < 0) continue;
        SaveBackup b{};
        struct tm tm{};
        if (sscanf(n.c_str(), "%4d%2d%2d-%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) == 5) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            char when[24];
            strftime(when, sizeof(when), "%d %b %Y  %H:%M", &tm);
            snprintf(b.label, sizeof(b.label), "%s  %s", when, KIND_NAME[k]);
        } else {
            snprintf(b.label, sizeof(b.label), "%.30s  %s", n.c_str(), KIND_NAME[k]);
        }
        snprintf(b.path, sizeof(b.path), "%s/%s", dir, n.c_str());
        b.ext = k == EXT;
        all.push_back({ n, b });
    }
    for (int k = 0; k < KINDS; k++) {
        std::string cp = cpTitleDir(t.tid, t.media, (Kind)k, false);
        if (cp.empty()) continue;
        for (auto& n : listDir(cp, true)) {
            SaveBackup b{};
            snprintf(b.label, sizeof(b.label), "%.24s  Checkpoint %s", n.c_str(), KIND_NAME[k]);
            snprintf(b.path, sizeof(b.path), "%s/%s", cp.c_str(), n.c_str());
            b.ext = k == EXT;
            b.checkpoint = true;
            all.push_back({ n, b });
        }
    }
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<SaveBackup> out;
    for (auto& a : all) out.push_back(a.second);
    return out;
}

bool restoreBackup(const SaveTitle& t, const SaveBackup& b, char* msg, size_t cap) {
    Kind k = b.ext ? EXT : SAVE;
    std::vector<u8> pk;
    if (b.checkpoint) {
        std::vector<SaveFile> files;
        size_t total = 0;
        if (!readRawTree(b.path, "", files, total) || files.empty()) { snprintf(msg, cap, "Checkpoint folder unreadable"); return false; }
        pk = pack(files);
    } else if (!readFile(b.path, pk)) {
        snprintf(msg, cap, "Backup unreadable");
        return false;
    }
    mkdirs(std::string(STORE_DIR) + "/in");
    if (!writeFile(storePath(t.tid, k, "in/").c_str(), pk)) { snprintf(msg, cap, "Card write failed"); return false; }
    switch (syncOne(t.tid, (FS_MediaType)t.media, k)) {
        case Outcome::Pulled: snprintf(msg, cap, "Restored, the replaced %s is the newest backup", KIND_NAME[k]); return true;
        case Outcome::Same:   snprintf(msg, cap, "Already the console's %s", KIND_NAME[k]); return true;
        case Outcome::NoSave: remove(storePath(t.tid, k, "in/").c_str()); snprintf(msg, cap, "No %s on the console yet, start the game once", KIND_NAME[k]); return false;
        default:              snprintf(msg, cap, "Restore failed, see log"); return false;
    }
}
