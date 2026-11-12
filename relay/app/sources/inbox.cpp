#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "ftp.h"
#include "inbox.hpp"
#include "update.hpp"

static const char* INBOX = "sdmc:/3ds/relay/inbox";

bool installInbox(char* msg, size_t cap) {
    std::string done = std::string(INBOX) + "/done";
    mkdir("sdmc:/3ds/relay", 0777);
    mkdir(INBOX, 0777);
    mkdir(done.c_str(), 0777);
    // Names first: renames while readdir walks the same directory can skip entries.
    std::vector<std::string> names;
    if (DIR* d = opendir(INBOX)) {
        while (dirent* e = readdir(d)) {
            size_t n = strlen(e->d_name);
            if (e->d_type != DT_DIR && n > 4 && !strcasecmp(e->d_name + n - 4, ".cia")) names.push_back(e->d_name);
        }
        closedir(d);
    }
    int ok = 0, failed = 0;
    for (auto& name : names) {
        std::string src = std::string(INBOX) + "/" + name, dst = done + "/" + name;
        char why[96];
        if (!installCiaFile(src.c_str(), why, sizeof(why))) { ftp_log("inbox %s: %s", name.c_str(), why); failed++; continue; }
        remove(dst.c_str());
        rename(src.c_str(), dst.c_str());
        ftp_log("inbox installed %s", name.c_str());
        ok++;
    }
    if (!ok && !failed) snprintf(msg, cap, "Inbox empty, put CIAs in /3ds/relay/inbox");
    else snprintf(msg, cap, "%d installed%s", ok, failed ? ", some failed, see log" : "");
    return !failed;
}
