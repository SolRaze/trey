/* Small FTP server on the SD card: anonymous, passive mode only, a few sessions at once.
 * Built into both the launcher app and the plugin; socInit and the sdmc mount come from the host. */
#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ftp.h"

#define MAX_SESSIONS 4
#define BUF_SIZE     0x10000
#define LINE_MAX_    1024
#define PASV_FIRST   5001
#define PASV_COUNT   100
#define IDLE_MS      (300 * 1000)
#define DATA_WAIT_MS 15000
#define SEND_STALL_MS 30000
#define RECV_STALL_MS 60000
#define STACK_SIZE   0x4000

typedef struct {
    int ctrl, pasv, data;
    u64 rest;
    char cwd[PATH_MAX];
    char rnfr[PATH_MAX];
    char line[LINE_MAX_];
    int line_len;
    char *buf;
} sess_t;

static volatile bool running;
static volatile int sessions;
static int listen_fd = -1;
static int thread_prio;
static char banner_line[64];
static Thread listener;
static int pasv_next;
static volatile unsigned long long total_rx, total_tx;
static char last_action[96];
static const ftp_hooks_t *hooks;
static char log_ring[FTP_LOG_LINES][96];
static volatile unsigned log_next;

void ftp_set_hooks(const ftp_hooks_t *h) { hooks = h; }

/* Lines may tear when two threads log at once; the index never does. */
void ftp_log(const char *fmt, ...) {
    unsigned i = __atomic_fetch_add(&log_next, 1, __ATOMIC_RELAXED) % FTP_LOG_LINES;
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    int n = snprintf(log_ring[i], sizeof(log_ring[i]), "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_ring[i] + n, sizeof(log_ring[i]) - n, fmt, ap);
    va_end(ap);
}

/* Oldest first, one line each. */
static size_t log_dump(char *out, size_t cap) {
    unsigned end = log_next, start = end > FTP_LOG_LINES ? end - FTP_LOG_LINES : 0;
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = start; i < end && used < cap; i++)
        used += snprintf(out + used, cap - used, "%s%s", used ? "\n" : "", log_ring[i % FTP_LOG_LINES]);
    if (!used) used = snprintf(out, cap, "Log empty");
    return used;
}

/* Keeps the tail of a long path: the file name is the part worth reading. */
static void note(const char *verb, const char *abs) {
    size_t room = sizeof(last_action) - strlen(verb) - 5, len = strlen(abs);
    if (len > room) snprintf(last_action, sizeof(last_action), "%s ...%s", verb, abs + len - room);
    else snprintf(last_action, sizeof(last_action), "%s %s", verb, abs);
    ftp_log("%s", last_action);
}

void ftp_status(ftp_status_t *out) {
    out->sessions = sessions;
    out->rx = total_rx;
    out->tx = total_tx;
    memcpy(out->last, last_action, sizeof(out->last));
    out->last[sizeof(out->last) - 1] = '\0';
}

int ftp_sessions(void) { return sessions; }

static void sock_bufs(int fd) {
    int sz = BUF_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
}

/* Every thread of the process shares one soc:U session, and a socket call that blocks holds
 * that session against all the others, the Artic server included. So nothing here may block
 * in a socket call: sockets are non-blocking, poll takes a zero timeout, waiting is a sleep. */
static void nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* Waits for fd to become readable, checking every step_ms. 1 ready, 0 timed out or stopping, -1 error. */
static int wait_in(int fd, int ms, int step_ms) {
    while (running && ms > 0) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 0);
        if (r < 0) return -1;
        if (r > 0) return (p.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : -1;
        svcSleepThread(step_ms * 1000000LL);
        ms -= step_ms;
    }
    return 0;
}

static bool send_all(int fd, const char *p, size_t n) {
    int stalled_ms = 0;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if ((errno != EAGAIN && errno != EWOULDBLOCK) || !running || ++stalled_ms > SEND_STALL_MS)
                return false;
            svcSleepThread(1000000);
            continue;
        }
        stalled_ms = 0;
        p += w; n -= (size_t)w;
        __atomic_add_fetch(&total_tx, (unsigned long long)w, __ATOMIC_RELAXED);
    }
    return true;
}

static void reply(sess_t *s, const char *fmt, ...) {
    char out[LINE_MAX_ + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(out) - 3) n = sizeof(out) - 3;
    out[n++] = '\r'; out[n++] = '\n';
    send_all(s->ctrl, out, n);
}

static void close_fd(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

/* Resolves arg against cwd into an absolute path with no ".", ".." or trailing slash. */
static void resolve(const char *cwd, const char *arg, char *out) {
    char tmp[PATH_MAX * 2];
    if (!arg || !*arg) snprintf(tmp, sizeof(tmp), "%s", cwd);
    else if (arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s", arg);
    else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);

    size_t len = 0;
    out[0] = '\0';
    for (char *save = NULL, *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            char *slash = strrchr(out, '/');
            if (slash) { *slash = '\0'; len = slash - out; }
            continue;
        }
        size_t tl = strlen(tok);
        if (len + 1 + tl >= PATH_MAX) break;
        out[len++] = '/';
        memcpy(out + len, tok, tl);
        len += tl;
        out[len] = '\0';
    }
    if (!len) strcpy(out, "/");
}

/* SD_PREFIX + absolute path; "sdmc:" is the libctru sdmc device, test/host.sh overrides it. */
#ifndef SD_PREFIX
#define SD_PREFIX "sdmc:"
#endif
static void sd_path(const char *abs, char *out) {
    snprintf(out, PATH_MAX + 8, SD_PREFIX "%s", abs);
}

static void resolve_sd(sess_t *s, const char *arg, char *abs, char *sd) {
    resolve(s->cwd, arg, abs);
    sd_path(abs, sd);
}

static void open_pasv(sess_t *s, bool extended) {
    close_fd(&s->pasv);
    for (int tries = 0; tries < PASV_COUNT; tries++) {
        int port = PASV_FIRST + (__atomic_fetch_add(&pasv_next, 1, __ATOMIC_RELAXED) % PASV_COUNT);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 1) == 0) {
            s->pasv = fd;
            nonblock(fd);
            u32 ip = (u32)gethostid();
            if (extended)
                reply(s, "229 Entering Extended Passive Mode (|||%d|)", port);
            else
                reply(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%d,%d)",
                      ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24, port >> 8, port & 0xff);
            return;
        }
        close(fd);
    }
    reply(s, "425 Cannot open passive port");
}

/* Sends 150, then accepts the client's data connection on the passive socket. */
static bool open_data(sess_t *s, const char *what) {
    if (s->pasv < 0) { reply(s, "425 Use PASV or EPSV first"); return false; }
    reply(s, "150 %s", what);
    if (wait_in(s->pasv, DATA_WAIT_MS, 5) != 1) {
        close_fd(&s->pasv);
        reply(s, "425 No data connection");
        return false;
    }
    s->data = accept(s->pasv, NULL, NULL);
    close_fd(&s->pasv);
    if (s->data < 0) { reply(s, "425 Accept failed"); return false; }
    sock_bufs(s->data);
    nonblock(s->data);
    return true;
}

/* Half-closes and waits for the client to close, so the tail of the stream is not dropped. */
static void finish_data(sess_t *s) {
    if (s->data < 0) return;
    shutdown(s->data, SHUT_WR);
    char tmp[64];
    for (int i = 0; i < 20 && wait_in(s->data, 100, 5) == 1; i++)
        if (recv(s->data, tmp, sizeof(tmp), 0) <= 0) break;
    close_fd(&s->data);
}

static const char *MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int list_line(char *out, size_t cap, const char *sd, const char *name, bool is_dir, u64 size) {
    u64 mt = 0;
    struct tm tm = { 0 };
    time_t t = archive_getmtime(sd, &mt) == 0 ? (time_t)mt : 0;
    gmtime_r(&t, &tm);
    /* Directories carry x so an lftp mirror can enter what it creates. */
    return snprintf(out, cap, "%s 1 3DS 3DS %llu %s %2d %5d %s\r\n",
                    is_dir ? "drwxrwxrwx" : "-rw-rw-rw-", (unsigned long long)size, MONTHS[tm.tm_mon % 12],
                    tm.tm_mday, tm.tm_year + 1900, name);
}

static void cmd_list(sess_t *s, const char *arg, bool names_only) {
    while (arg && *arg == '-') {           /* ls flags: "-la", "-a /path" */
        arg = strchr(arg, ' ');
        if (arg) while (*arg == ' ') arg++;
    }
    char abs[PATH_MAX], sd[PATH_MAX + 8];
    resolve_sd(s, arg, abs, sd);
    struct stat st;
    if (stat(sd, &st) != 0) { close_fd(&s->pasv); reply(s, "550 %s", strerror(errno)); return; }

    if (!S_ISDIR(st.st_mode)) {
        if (!open_data(s, "Listing")) return;
        const char *name = strrchr(abs, '/') + 1;
        int n = names_only ? snprintf(s->buf, BUF_SIZE, "%s\r\n", name)
                           : list_line(s->buf, BUF_SIZE, sd, name, false, st.st_size);
        bool ok = send_all(s->data, s->buf, n);
        finish_data(s);
        reply(s, ok ? "226 Done" : "426 Transfer aborted");
        return;
    }

    DIR *d = opendir(sd);
    if (!d) { close_fd(&s->pasv); reply(s, "550 %s", strerror(errno)); return; }
    if (!open_data(s, "Listing")) { closedir(d); return; }

    size_t used = 0;
    bool ok = true;
    char child[PATH_MAX + 8 + NAME_MAX + 2];
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (used > BUF_SIZE - PATH_MAX - 128) {
            ok = send_all(s->data, s->buf, used);
            used = 0;
        }
        if (names_only) {
            used += snprintf(s->buf + used, BUF_SIZE - used, "%s\r\n", e->d_name);
            continue;
        }
        snprintf(child, sizeof(child), "%s%s%s", sd, sd[strlen(sd) - 1] == '/' ? "" : "/", e->d_name);
        struct stat cs = { 0 };
        bool dir = e->d_type == DT_DIR;
        if (!dir && stat(child, &cs) != 0) cs.st_size = 0;
        used += list_line(s->buf + used, BUF_SIZE - used, child, e->d_name, dir, dir ? 0 : cs.st_size);
    }
    closedir(d);
    if (ok && used) ok = send_all(s->data, s->buf, used);
    finish_data(s);
    reply(s, ok ? "226 Done" : "426 Transfer aborted");
}

static void cmd_retr(sess_t *s, const char *arg) {
    char abs[PATH_MAX], sd[PATH_MAX + 8];
    resolve_sd(s, arg, abs, sd);
    int fd = open(sd, O_RDONLY);
    u64 rest = s->rest;
    s->rest = 0;
    if (fd < 0) { close_fd(&s->pasv); reply(s, "550 %s", strerror(errno)); return; }
    if (rest && lseek(fd, (off_t)rest, SEEK_SET) < 0) {
        close(fd); close_fd(&s->pasv); reply(s, "550 Seek failed"); return;
    }
    if (!open_data(s, "Sending")) { close(fd); return; }
    note("get", abs);
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, s->buf, BUF_SIZE);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!send_all(s->data, s->buf, n)) { ok = false; break; }
    }
    close(fd);
    finish_data(s);
    reply(s, ok ? "226 Done" : "426 Transfer aborted");
}

static void cmd_stor(sess_t *s, const char *arg) {
    char abs[PATH_MAX], sd[PATH_MAX + 8];
    resolve_sd(s, arg, abs, sd);
    u64 rest = s->rest;
    s->rest = 0;

    /* A host sink takes the stream instead of a file, e.g. a CIA straight into the installer. */
    void *ctx = NULL;
    const char *err = NULL;
    int take = hooks && hooks->sink_open ? hooks->sink_open(abs, &ctx, &err) : 0;
    if (take < 0 || (take && rest)) {
        if (take) hooks->sink_close(ctx, 0);
        close_fd(&s->pasv);
        reply(s, "550 %s", take < 0 && err ? err : "Cannot resume this upload");
        return;
    }
    int fd = -1;
    if (!take) {
        fd = open(sd, O_WRONLY | O_CREAT | (rest ? 0 : O_TRUNC), 0666);
        if (fd < 0) { close_fd(&s->pasv); reply(s, "550 %s", strerror(errno)); return; }
        if (rest && lseek(fd, (off_t)rest, SEEK_SET) < 0) {
            close(fd); close_fd(&s->pasv); reply(s, "550 Seek failed"); return;
        }
    }
    if (!open_data(s, "Receiving")) {
        if (take) hooks->sink_close(ctx, 0); else close(fd);
        return;
    }
    note(take ? "install" : "put", abs);
    bool ok = true;
    /* Fills the buffer before each card write: recv hands over a packet or two at a time,
     * and one FS call per packet is what makes an upload slow. */
    size_t have = 0;
    int stalled_ms = 0;
    for (;;) {
        ssize_t n = recv(s->data, s->buf + have, BUF_SIZE - have, 0);
        if (n < 0) {
            if ((errno != EAGAIN && errno != EWOULDBLOCK) || !running || ++stalled_ms > RECV_STALL_MS) {
                ok = false; break;
            }
            svcSleepThread(1000000);
            continue;
        }
        stalled_ms = 0;
        have += (size_t)n;
        __atomic_add_fetch(&total_rx, (unsigned long long)n, __ATOMIC_RELAXED);
        if (n == 0 || have == BUF_SIZE) {
            if (have && !(take ? hooks->sink_write(ctx, s->buf, have) : write(fd, s->buf, have) == (ssize_t)have)) {
                ok = false; break;
            }
            have = 0;
            if (n == 0) break;
        }
    }
    close_fd(&s->data);
    if (take) {
        err = hooks->sink_close(ctx, ok);
        if (ok && err) ok = false;
        if (!ok) ftp_log("install failed: %s", err ? err : "transfer");
        reply(s, ok ? "226 Installed" : "426 %s", err ? err : "Transfer failed");
        return;
    }
    if (close(fd) != 0) ok = false;
    reply(s, ok ? "226 Done" : "426 Transfer failed");
}

/* Multi-line text goes out as "code-first", " middle", "code End". */
static void reply_text(sess_t *s, int code, const char *text) {
    if (!strchr(text, '\n')) { reply(s, "%d %s", code, text); return; }
    char *out = s->buf + BUF_SIZE / 2;
    size_t cap = BUF_SIZE / 2 - 32, used = snprintf(out, cap, "%d-", code);
    for (const char *p = text; *p && used < cap - 2; p++) {
        if (*p == '\n') { out[used++] = '\r'; out[used++] = '\n'; if (used < cap) out[used++] = ' '; }
        else out[used++] = *p;
    }
    used += snprintf(out + used, BUF_SIZE / 2 - used, "\r\n%d End\r\n", code);
    send_all(s->ctrl, out, used);
}

static void cmd_site(sess_t *s, char *arg) {
    char *out = s->buf;
    size_t cap = BUF_SIZE / 2;
    if (!strncasecmp(arg, "LOG", 3) && (!arg[3] || arg[3] == ' ')) {
        log_dump(out, cap);
        reply_text(s, 200, out);
        return;
    }
    out[0] = '\0';
    int code = hooks && hooks->site ? hooks->site(arg, out, cap) : 0;
    if (!code) reply(s, "500 Unknown SITE command; SITE HELP");
    else reply_text(s, code, out[0] ? out : "OK");
}

static void cmd_simple(sess_t *s, const char *cmd, const char *arg) {
    char abs[PATH_MAX], sd[PATH_MAX + 8];
    struct stat st;

    if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD") || !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
        bool up = !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP");
        resolve_sd(s, up ? ".." : arg, abs, sd);
        if (stat(sd, &st) == 0 && S_ISDIR(st.st_mode)) { strcpy(s->cwd, abs); reply(s, "250 OK"); }
        else reply(s, "550 No such directory");
    } else if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        char q[PATH_MAX * 2];
        size_t j = 0;
        for (const char *p = s->cwd; *p && j < sizeof(q) - 2; p++) {
            if (*p == '"') q[j++] = '"';
            q[j++] = *p;
        }
        q[j] = '\0';
        reply(s, "257 \"%s\" is the current directory", q);
    } else if (!strcmp(cmd, "SIZE")) {
        resolve_sd(s, arg, abs, sd);
        if (stat(sd, &st) == 0 && !S_ISDIR(st.st_mode)) reply(s, "213 %llu", (unsigned long long)st.st_size);
        else reply(s, "550 No such file");
    } else if (!strcmp(cmd, "MDTM")) {
        resolve_sd(s, arg, abs, sd);
        u64 mt;
        if (archive_getmtime(sd, &mt) == 0) {
            time_t t = (time_t)mt;
            struct tm tm;
            gmtime_r(&t, &tm);
            reply(s, "213 %04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else reply(s, "550 No such file");
    } else if (!strcmp(cmd, "DELE")) {
        resolve_sd(s, arg, abs, sd);
        if (unlink(sd) == 0) { note("rm", abs); reply(s, "250 Deleted"); } else reply(s, "550 %s", strerror(errno));
    } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
        resolve_sd(s, arg, abs, sd);
        if (mkdir(sd, 0777) == 0) reply(s, "257 \"%s\" created", abs); else reply(s, "550 %s", strerror(errno));
    } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
        resolve_sd(s, arg, abs, sd);
        if (rmdir(sd) == 0) reply(s, "250 Removed"); else reply(s, "550 %s", strerror(errno));
    } else if (!strcmp(cmd, "RNFR")) {
        resolve_sd(s, arg, abs, sd);
        if (stat(sd, &st) == 0) { strcpy(s->rnfr, sd); reply(s, "350 Ready for RNTO"); }
        else reply(s, "550 No such file");
    } else if (!strcmp(cmd, "RNTO")) {
        resolve_sd(s, arg, abs, sd);
        if (!s->rnfr[0]) reply(s, "503 RNFR first");
        else if (rename(s->rnfr, sd) == 0) { note("mv", abs); reply(s, "250 Renamed"); }
        else reply(s, "550 %s", strerror(errno));
        s->rnfr[0] = '\0';
    } else if (!strcmp(cmd, "REST")) {
        s->rest = strtoull(arg, NULL, 10);
        reply(s, "350 Restarting at %llu", (unsigned long long)s->rest);
    } else if (!strcmp(cmd, "FEAT")) {
        reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n EPSV\r\n PASV\r\n UTF8\r\n SITE\r\n211 End");
    } else if (!strcmp(cmd, "USER")) reply(s, "230 Logged in");
    else if (!strcmp(cmd, "PASS")) reply(s, "230 Logged in");
    else if (!strcmp(cmd, "SYST")) reply(s, "215 UNIX Type: L8");
    else if (!strcmp(cmd, "TYPE") || !strcmp(cmd, "MODE") || !strcmp(cmd, "STRU") || !strcmp(cmd, "OPTS"))
        reply(s, "200 OK");
    else if (!strcmp(cmd, "NOOP")) reply(s, "200 OK");
    else if (!strcmp(cmd, "ALLO")) reply(s, "202 Not needed");
    else if (!strcmp(cmd, "ABOR")) { close_fd(&s->pasv); reply(s, "225 No transfer"); }
    else if (!strcmp(cmd, "PORT") || !strcmp(cmd, "EPRT")) reply(s, "502 Passive mode only");
    else reply(s, "502 Not implemented");
}

/* Returns false when the session should end. */
static bool dispatch(sess_t *s, char *line) {
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = '\0'; } else arg = line + strlen(line);
    for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

    if (!strcmp(line, "QUIT")) { reply(s, "221 Bye"); return false; }
    if (!strcmp(line, "PASV")) open_pasv(s, false);
    else if (!strcmp(line, "EPSV")) {
        if (!strcasecmp(arg, "ALL")) reply(s, "200 OK"); else open_pasv(s, true);
    }
    else if (!strcmp(line, "LIST")) cmd_list(s, arg, false);
    else if (!strcmp(line, "NLST")) cmd_list(s, arg, true);
    else if (!strcmp(line, "RETR")) cmd_retr(s, arg);
    else if (!strcmp(line, "STOR")) cmd_stor(s, arg);
    else if (!strcmp(line, "SITE")) cmd_site(s, arg);
    else cmd_simple(s, line, arg);
    return true;
}

/* Reads one CRLF line into s->line. 1 line ready, 0 stop or idle, -1 closed. */
static int read_line(sess_t *s) {
    int idle = 0;
    for (;;) {
        char *nl = memchr(s->line, '\n', s->line_len);
        if (nl) return 1;
        if (s->line_len >= LINE_MAX_ - 1) s->line_len = 0;  /* overlong line, drop it */
        int r = wait_in(s->ctrl, 1000, 20);
        if (!running) return 0;
        if (r < 0) return -1;
        if (r == 0) {
            if ((idle += 1000) >= IDLE_MS) return 0;
            continue;
        }
        ssize_t n = recv(s->ctrl, s->line + s->line_len, LINE_MAX_ - 1 - s->line_len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        s->line_len += n;
    }
}

static void session(void *arg) {
    sess_t *s = arg;
    reply(s, banner_line);
    while (running && read_line(s) == 1) {
        char *nl = memchr(s->line, '\n', s->line_len);
        size_t len = nl - s->line;
        char cmd[LINE_MAX_];
        memcpy(cmd, s->line, len);
        cmd[len] = '\0';
        if (len && cmd[len - 1] == '\r') cmd[len - 1] = '\0';
        s->line_len -= len + 1;
        memmove(s->line, nl + 1, s->line_len);
        if (!dispatch(s, cmd)) break;
    }
    close_fd(&s->data);
    close_fd(&s->pasv);
    close_fd(&s->ctrl);
    free(s->buf);
    free(s);
    __atomic_sub_fetch(&sessions, 1, __ATOMIC_SEQ_CST);
}

static int open_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(FTP_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    nonblock(fd);
    return fd;
}

static void listen_loop(void *unused) {
    (void)unused;
    while (running) {
        if (listen_fd < 0 && (listen_fd = open_listener()) < 0) {
            svcSleepThread(1000000000LL);   /* port still held after a quick relaunch */
            continue;
        }
        if (wait_in(listen_fd, 500, 50) != 1) continue;
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0) { close_fd(&listen_fd); continue; }
        sess_t *s = calloc(1, sizeof(sess_t));
        char *buf = s ? malloc(BUF_SIZE) : NULL;
        if (!buf || sessions >= MAX_SESSIONS) {
            const char *busy = "421 Too many sessions\r\n";
            send(c, busy, strlen(busy), 0);
            close(c);
            free(s);
            free(buf);
            continue;
        }
        nonblock(c);
        s->ctrl = c; s->pasv = -1; s->data = -1; s->buf = buf;
        strcpy(s->cwd, "/");
        sock_bufs(c);
        __atomic_add_fetch(&sessions, 1, __ATOMIC_SEQ_CST);
        if (!threadCreate(session, s, STACK_SIZE, thread_prio, -2, true)) {
            __atomic_sub_fetch(&sessions, 1, __ATOMIC_SEQ_CST);
            close(c); free(buf); free(s);
        }
    }
    close_fd(&listen_fd);
}

int ftp_start(int prio, const char *banner) {
    if (listener) return 0;
    snprintf(banner_line, sizeof(banner_line), "220 %s", banner);
    running = true;
    thread_prio = prio;
    listener = threadCreate(listen_loop, NULL, STACK_SIZE, prio, -2, false);
    return listener ? 0 : -1;
}

void ftp_stop(void) {
    if (!listener) return;
    running = false;
    threadJoin(listener, U64_MAX);
    threadFree(listener);
    listener = NULL;
    for (int i = 0; i < 200 && sessions > 0; i++) svcSleepThread(10000000LL);  /* sessions are detached */
}
