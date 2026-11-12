#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>
#include <algorithm>

#include "ftp.h"
#include "saves.hpp"

// Two-way save sync between each title's save and its packed copy on the card,
// sdmc:/3ds/relay/saves/<TID>.sav, which a computer reads and writes over FTP (`trey saves`).
// sdmc:/3ds/relay/sync/<TID> holds the hash both sides had after the last sync, so each side's change
// since then is known: one side changed = that side wins; both changed, or never synced = the console
// wins. Whatever gets replaced is kept first as saves/old/<TID>.sav, one copy per title.
// Pack form: per file u16 path length, path, u32 size, data, all little-endian, sorted by path; u16 0 ends.
// Hash: 64-bit FNV-1a of the whole pack.

static const char* STATE_DIR = "sdmc:/3ds/relay/sync";
static const char* STORE_DIR = "sdmc:/3ds/relay/saves";

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

static bool openSave(u64 tid, FS_MediaType media, FS_Archive* arch) {
    u32 path[3] = { media, (u32)tid, (u32)(tid >> 32) };
    return R_SUCCEEDED(FSUSER_OpenArchive(arch, ARCHIVE_USER_SAVEDATA, (FS_Path){ PATH_BINARY, 12, path }));
}

// dir is "" for the root or "a/b"; files come out as "a/b/name".
static bool readTree(FS_Archive arch, const std::string& dir, std::vector<SaveFile>& out) {
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
            if (!readTree(arch, rel, out)) { ok = false; break; }
            continue;
        }
        Handle f;
        if (R_FAILED(FSUSER_OpenFile(&f, arch, utf16Path("/" + rel, pb), FS_OPEN_READ, 0))) { ok = false; break; }
        u64 size = 0;
        FSFILE_GetSize(f, &size);
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

// Replaces the archive's contents with files, then commits. Secure value cleared for SD titles so the
// game takes a save it did not write itself.
static bool writeSave(u64 tid, FS_MediaType media, const std::vector<SaveFile>& files) {
    FS_Archive arch;
    if (!openSave(tid, media, &arch)) return false;
    std::vector<u16> pb;
    bool ok = true;
    Handle d;
    if (R_SUCCEEDED(FSUSER_OpenDirectory(&d, arch, utf16Path("/", pb)))) {
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
    for (size_t k = 0; ok && k < files.size(); k++) {
        const std::string& p = files[k].path;
        for (size_t s = p.find('/'); s != std::string::npos; s = p.find('/', s + 1))
            FSUSER_CreateDirectory(arch, utf16Path("/" + p.substr(0, s), pb), 0);   // exists is fine
        FS_Path fp = utf16Path("/" + p, pb);
        Handle f;
        if (R_FAILED(FSUSER_CreateFile(arch, fp, 0, files[k].data.size())) ||
            R_FAILED(FSUSER_OpenFile(&f, arch, fp, FS_OPEN_WRITE, 0))) { ok = false; break; }
        u32 w = 0;
        Result r = files[k].data.empty() ? 0
                 : FSFILE_Write(f, &w, 0, files[k].data.data(), files[k].data.size(), FS_WRITE_FLUSH);
        FSFILE_Close(f);
        if (R_FAILED(r) || w != files[k].data.size()) ok = false;
    }
    if (ok) ok = R_SUCCEEDED(FSUSER_ControlArchive(arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA, nullptr, 0, nullptr, 0));
    FSUSER_CloseArchive(arch);
    if (ok && media == MEDIATYPE_SD) {
        u64 in = ((u64)SECUREVALUE_SLOT_SD << 32) | (((u32)tid >> 8) & 0xFFFFF) << 8;
        u8 out;
        FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &in, 8, &out, 1);
    }
    return ok;
}

static u64 lastHash(u64 tid) {
    char p[64];
    snprintf(p, sizeof(p), "%s/%016llX", STATE_DIR, tid);
    FILE* f = fopen(p, "r");
    unsigned long long h = 0;
    if (f) { if (fscanf(f, "%llx", &h) != 1) h = 0; fclose(f); }
    return h;
}

static void setLast(u64 tid, u64 h) {
    char p[64];
    mkdir("sdmc:/3ds/relay", 0777);
    mkdir(STATE_DIR, 0777);
    snprintf(p, sizeof(p), "%s/%016llX", STATE_DIR, tid);
    FILE* f = fopen(p, "w");
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
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (!ok || rename(tmp, path)) { remove(tmp); return false; }
    return true;
}

enum class Outcome { Same, Pushed, Pulled, NoSave, Failed };

static Outcome syncOne(u64 tid, FS_MediaType media) {
    FS_Archive arch;
    if (!openSave(tid, media, &arch)) return Outcome::NoSave;
    std::vector<SaveFile> files;
    bool read = readTree(arch, "", files);
    FSUSER_CloseArchive(arch);
    if (!read) { ftp_log("save %016llX unreadable", tid); return Outcome::Failed; }
    if (files.empty()) return Outcome::NoSave;
    std::vector<u8> local = pack(files);
    u64 lh = fnv(local), last = lastHash(tid);

    char store[64], old[64];
    snprintf(store, sizeof(store), "%s/%016llX.sav", STORE_DIR, tid);
    snprintf(old, sizeof(old), "%s/old/%016llX.sav", STORE_DIR, tid);
    std::vector<u8> remote;
    bool have = readFile(store, remote);
    u64 rh = have ? fnv(remote) : 0;

    if (have && rh == lh) { if (last != lh) setLast(tid, lh); return Outcome::Same; }
    bool pull = have && lh == last && rh != last;   // only the card copy moved
    if (!pull) {
        if (have && rh != last && !writeFile(old, remote)) { ftp_log("save %016llX: backup failed", tid); return Outcome::Failed; }
        if (!writeFile(store, local)) { ftp_log("save %016llX: write failed", tid); return Outcome::Failed; }
        setLast(tid, lh);
        ftp_log("save %016llX sent", tid);
        return Outcome::Pushed;
    }
    std::vector<SaveFile> in;
    if (!writeFile(old, local)) { ftp_log("save %016llX: backup failed", tid); return Outcome::Failed; }
    if (!unpack(remote.data(), remote.size(), in) || !writeSave(tid, media, in)) {
        ftp_log("save %016llX: restore failed", tid);
        return Outcome::Failed;
    }
    setLast(tid, rh);
    ftp_log("save %016llX restored", tid);
    return Outcome::Pulled;
}

bool syncSaves(char* msg, size_t cap) {
    if (R_FAILED(amInit())) { snprintf(msg, cap, "AM unavailable"); return false; }
    mkdir("sdmc:/3ds/relay", 0777);
    mkdir(STORE_DIR, 0777);
    mkdir((std::string(STORE_DIR) + "/old").c_str(), 0777);
    int pushed = 0, pulled = 0, same = 0, failed = 0;
    static const FS_MediaType media[] = { MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
    for (FS_MediaType m : media) {
        u32 count = 0, got = 0;
        if (R_FAILED(AM_GetTitleCount(m, &count)) || !count) continue;
        std::vector<u64> ids(count);
        if (R_FAILED(AM_GetTitleList(&got, m, count, ids.data()))) continue;
        for (u32 i = 0; i < got; i++) {
            if ((ids[i] >> 32) != 0x00040000) continue;   // applications only
            switch (syncOne(ids[i], m)) {
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
