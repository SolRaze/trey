/* os.h on libctru, for the console, plus the soc wrappers port/include/lwip/sockets.h routes
 * microlink's socket calls through.
 *
 * Soc rule: one soc:U session per process, and a socket call that blocks holds it against every
 * other thread (FTP sessions, Artic, the tunnel). So every socket is non-blocking in soc; a call
 * the caller made blocking is a poll with timeout 0 and a sleep, bounded by its SO_RCVTIMEO or
 * SO_SNDTIMEO (soc has neither, they are kept here). The one call that still blocks is
 * getaddrinfo, once per dial, as tunnel.c and libcurl do.
 *
 * Embedder: socInit and sslcInit before os_init. os_init picks the thread priority: one below
 * the calling thread, all engine threads equal, so every wait sleeps rather than spins. */
#include "os.h"

#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/sockets.h"   /* the ts_* prototypes; its macros are undone below */
#undef socket
#undef connect
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef setsockopt
#undef fcntl
#undef select
#undef close
#undef read
#undef write
#undef getaddrinfo

/* ---- time ---- */

uint64_t os_us(void) {
    uint64_t t = svcGetSystemTick();
    return t / SYSCLOCK_ARM11 * 1000000u + t % SYSCLOCK_ARM11 * 1000000u / SYSCLOCK_ARM11;
}

uint64_t os_ms(void) { return os_us() / 1000u; }
void os_sleep_ms(uint32_t ms) { svcSleepThread((s64)ms * 1000000LL); }

/* ---- threads ---- */

static int prio = 0x30;

int os_init(void) {
    s32 p;
    if (R_SUCCEEDED(svcGetThreadPriority(&p, CUR_THREAD_HANDLE))) prio = p < 0x3F ? p + 1 : 0x3F;
    return 0;
}

struct os_thread { void (*fn)(void *); void *arg; };
static __thread struct os_thread *self;

static void trampoline(void *p) {
    self = p;
    self->fn(self->arg);
    os_thread_exit();
}

void os_thread_exit(void) {
    free(self);
    self = NULL;
    threadExit(0);
    __builtin_unreachable();
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg, size_t stack, int p) {
    (void)p;
    struct os_thread *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    /* ESP sizes assume ESP-IDF's lean libc; mbedTLS handshakes on newlib want more. */
    size_t sz = stack * 2 < 32 * 1024 ? 32 * 1024 : stack * 2;
    if (!threadCreate(trampoline, t, sz, prio, -2, true)) { free(t); return NULL; }
    return t;
}

/* ---- locks ---- */

struct os_mutex { bool rec; LightLock l; RecursiveLock r; };
struct os_cond { CondVar c; };

os_mutex_t *os_mutex_new(bool recursive) {
    struct os_mutex *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->rec = recursive;
    if (recursive) RecursiveLock_Init(&m->r);
    else LightLock_Init(&m->l);
    return m;
}

void os_mutex_free(os_mutex_t *m) { free(m); }
void os_mutex_lock(os_mutex_t *m) { if (m->rec) RecursiveLock_Lock(&m->r); else LightLock_Lock(&m->l); }
void os_mutex_unlock(os_mutex_t *m) { if (m->rec) RecursiveLock_Unlock(&m->r); else LightLock_Unlock(&m->l); }

os_cond_t *os_cond_new(void) {
    struct os_cond *c = malloc(sizeof *c);
    if (c) CondVar_Init(&c->c);
    return c;
}

void os_cond_free(os_cond_t *c) { free(c); }
void os_cond_broadcast(os_cond_t *c) { CondVar_Broadcast(&c->c); }

/* m must be non-recursive: CondVar waits on a LightLock. */
bool os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms) {
    if (timeout_ms == UINT32_MAX) { CondVar_Wait(&c->c, &m->l); return true; }
    return CondVar_WaitTimeout(&c->c, &m->l, (s64)timeout_ms * 1000000LL) == 0;
}

/* ---- random, log ---- */

/* sslc's RNG, the same source the portlib's mbedtls_hardware_poll reads. A failure here would
 * mean predictable keys, so it stops the process rather than return weak bytes. */
void os_random(void *buf, size_t len) {
    if (R_FAILED(sslcGenerateRandomData(buf, len))) svcBreak(USERBREAK_PANIC);
}

static void (*log_fn)(const char *);
void os_set_log(void (*fn)(const char *line)) { log_fn = fn; }
void os_log_line(const char *line) { if (log_fn) log_fn(line); }

/* ---- soc wrappers ---- */

#define NFD 64
#define STEP_MS 5
#define CONNECT_MS 20000   /* a blocking connect with neither timeout set */

/* Per socket: the caller's view. In soc every one of these is O_NONBLOCK. */
static struct { bool sock, nb; uint32_t rcv_ms, snd_ms; } fds[NFD];

static bool ours(int fd) { return fd >= 0 && fd < NFD && fds[fd].sock; }
static uint32_t rcv_ms(int fd) { return ours(fd) ? fds[fd].rcv_ms : 0; }
static uint32_t snd_ms(int fd) { return ours(fd) ? fds[fd].snd_ms : 0; }
static bool again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }

/* 1 ready, 0 timed out, -1 error. ms 0 = no limit. */
static int wait_fd(int fd, short ev, uint32_t ms) {
    uint64_t until = ms ? os_ms() + ms : UINT64_MAX;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = ev };
        int r = poll(&p, 1, 0);
        if (r < 0) return -1;
        if (r > 0) return 1;   /* POLLERR / POLLHUP: the call itself reports it */
        if (os_ms() >= until) return 0;
        svcSleepThread(STEP_MS * 1000000LL);
    }
}

int ts_socket(int dom, int type, int proto) {
    int fd = socket(dom, type, proto);
    if (fd < 0) return fd;
    if (fd >= NFD) { close(fd); errno = EMFILE; return -1; }
    fds[fd].sock = true;
    fds[fd].nb = false;
    fds[fd].rcv_ms = fds[fd].snd_ms = 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

int ts_close(int fd) {
    if (ours(fd)) fds[fd].sock = false;
    return close(fd);
}

int ts_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    if (!ours(fd)) return fcntl(fd, cmd, arg);
    if (cmd == F_GETFL) {
        int f = fcntl(fd, F_GETFL, 0);
        return f < 0 ? f : (f & ~O_NONBLOCK) | (fds[fd].nb ? O_NONBLOCK : 0);
    }
    if (cmd == F_SETFL) {
        fds[fd].nb = arg & O_NONBLOCK;
        return fcntl(fd, F_SETFL, arg | O_NONBLOCK);
    }
    return fcntl(fd, cmd, arg);
}

int ts_setsockopt(int fd, int level, int name, const void *v, socklen_t len) {
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && ours(fd)) {
        if (len < sizeof(struct timeval)) { errno = EINVAL; return -1; }
        const struct timeval *tv = v;
        uint32_t ms = (uint32_t)tv->tv_sec * 1000u + (uint32_t)tv->tv_usec / 1000u;
        if (!ms && tv->tv_usec) ms = 1;
        if (name == SO_RCVTIMEO) fds[fd].rcv_ms = ms; else fds[fd].snd_ms = ms;
        return 0;
    }
    if (level == SOL_SOCKET && name == SO_KEEPALIVE) return 0;
    if (level == IPPROTO_TCP && (name == TCP_KEEPIDLE || name == TCP_KEEPINTVL || name == TCP_KEEPCNT))
        return 0;   /* soc has no keepalive options; microlink's own pings cover it */
    return setsockopt(fd, level, name, v, len);
}

/* soc reports POLLOUT before the handshake ends and SO_ERROR as a raw negative SOC code
 * (-26 = in progress), so completion is read by repeating connect: EISCONN = done. */
int ts_connect(int fd, const struct sockaddr *a, socklen_t len) {
    int r = connect(fd, a, len);
    if (r == 0 || !ours(fd) || fds[fd].nb || !again()) return r;
    uint32_t ms = fds[fd].snd_ms ? fds[fd].snd_ms : fds[fd].rcv_ms ? fds[fd].rcv_ms : CONNECT_MS;
    uint64_t until = os_ms() + ms;
    for (;;) {
        r = connect(fd, a, len);
        if (r == 0 || errno == EISCONN) return 0;
        if (errno != EALREADY && !again()) return -1;
        if (os_ms() >= until) { errno = ETIMEDOUT; return -1; }
        svcSleepThread(STEP_MS * 1000000LL);
    }
}

/* One I/O call made blocking by polling: retried while it would block, until fd's timeout. */
#define BLOCKING(fd, ev, ms, call)                                          \
    for (;;) {                                                              \
        ssize_t r_ = (call);                                                \
        if (r_ >= 0 || !ours(fd) || fds[fd].nb || nb_flag || !again())     \
            return r_;                                                      \
        int w_ = wait_fd(fd, ev, ms);                                       \
        if (w_ == 0) { errno = EAGAIN; return -1; }                         \
        if (w_ < 0) return -1;                                              \
    }

ssize_t ts_recv(int fd, void *b, size_t n, int fl) {
    bool nb_flag = fl & MSG_DONTWAIT;
    fl &= ~MSG_DONTWAIT;
    BLOCKING(fd, POLLIN, rcv_ms(fd), recv(fd, b, n, fl));
}

ssize_t ts_recvfrom(int fd, void *b, size_t n, int fl, struct sockaddr *a, socklen_t *al) {
    bool nb_flag = fl & MSG_DONTWAIT;
    fl &= ~MSG_DONTWAIT;
    BLOCKING(fd, POLLIN, rcv_ms(fd), recvfrom(fd, b, n, fl, a, al));
}

ssize_t ts_send(int fd, const void *b, size_t n, int fl) {
    bool nb_flag = fl & MSG_DONTWAIT;
    fl &= ~MSG_DONTWAIT;
    BLOCKING(fd, POLLOUT, snd_ms(fd), send(fd, b, n, fl));
}

ssize_t ts_sendto(int fd, const void *b, size_t n, int fl, const struct sockaddr *a, socklen_t al) {
    bool nb_flag = fl & MSG_DONTWAIT;
    fl &= ~MSG_DONTWAIT;
    BLOCKING(fd, POLLOUT, snd_ms(fd), sendto(fd, b, n, fl, a, al));
}

ssize_t ts_read(int fd, void *b, size_t n) {
    if (!ours(fd)) return read(fd, b, n);
    return ts_recv(fd, b, n, 0);
}

ssize_t ts_write(int fd, const void *b, size_t n) {
    if (!ours(fd)) return write(fd, b, n);
    return ts_send(fd, b, n, 0);
}

/* select over poll(0) + sleep. Only sockets from ts_socket may be in the sets. */
int ts_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
    struct pollfd p[NFD];
    int n = 0;
    for (int fd = 0; fd < nfds && fd < NFD; fd++) {
        short ev = (rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0) |
                   (ex && FD_ISSET(fd, ex) ? POLLPRI : 0);
        if (ev) p[n++] = (struct pollfd){ .fd = fd, .events = ev };
    }
    uint64_t until = tv ? os_ms() + (uint64_t)tv->tv_sec * 1000u + (uint64_t)tv->tv_usec / 1000u : UINT64_MAX;
    for (;;) {
        int r = n ? poll(p, n, 0) : 0;
        if (r < 0) return -1;
        if (r > 0 || os_ms() >= until) {
            if (rd) FD_ZERO(rd);
            if (wr) FD_ZERO(wr);
            if (ex) FD_ZERO(ex);
            int ready = 0;
            for (int i = 0; i < n && r > 0; i++) {
                short re = p[i].revents;
                bool any = false;
                if (rd && (p[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, rd); any = true; }
                if (wr && (p[i].events & POLLOUT) && (re & (POLLOUT | POLLHUP | POLLERR))) { FD_SET(p[i].fd, wr); any = true; }
                if (ex && (p[i].events & POLLPRI) && (re & POLLPRI)) { FD_SET(p[i].fd, ex); any = true; }
                ready += any;
            }
            return ready;
        }
        svcSleepThread(STEP_MS * 1000000LL);
    }
}

/* soc resolves IPv4 only: AF_UNSPEC is asked as AF_INET. */
int ts_getaddrinfo(const char *host, const char *svc, const struct addrinfo *hints, struct addrinfo **res) {
    struct addrinfo h = hints ? *hints : (struct addrinfo){ 0 };
    if (h.ai_family == AF_UNSPEC) h.ai_family = AF_INET;
    return getaddrinfo(host, svc, &h, res);
}
