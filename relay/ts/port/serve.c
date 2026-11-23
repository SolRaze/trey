/* Serves the console's own TCP ports on its tailnet address: lwIP listens on 100.x for each port
 * in [lo, hi] and every accepted connection is bridged to a soc connection to the same port on
 * the console (127.0.0.1, then its LAN address). ftp.c stays unchanged: control 5000, PASV
 * 5001-5100. EPSV replies carry only a port, so they pass as is; a 227 reply carries ftp.c's LAN
 * address and is rewritten to the tailnet one on the way out.
 *
 * Threads: lwIP raw callbacks run on tcpip_thread and only queue; one bridge thread does all soc
 * I/O and all tcp_* calls under LOCK_TCPIP_CORE. Every soc socket is non-blocking, so the lock is
 * never held across a wait. Flow control: received pbufs are acknowledged (tcp_recved) only as
 * soc accepts them, so a slow card write slows the sender instead of filling lwIP's heap. */
#include <3ds.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include "os.h"
#include "ts_node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNS      12          /* within MEMP_NUM_TCP_PCB */
#define BUF        4096
#define DIAL_MS    3000
#define BUSY_MS    2
#define IDLE_MS    10

enum { FREE, NEW, DIALING, OPEN };

typedef struct {
    int state;
    struct tcp_pcb *pcb;   /* NULL once lwIP freed it (on_err) or it was closed */
    u16 port;
    int fd, tries;
    u64 t0;
    struct pbuf *rx;       /* from the tailnet, not yet written to soc */
    bool peer_fin, local_fin, fd_shut, dead;
    u8 pend[BUF + 32];     /* from soc, not yet accepted by tcp_write */
    size_t pend_len;
} conn_t;

static conn_t conns[CONNS];
static struct tcp_pcb *listeners[128];
static int nlisten;
static u16 ctrl_port;
static char tail_ip[20];   /* "a,b,c,d" for 227 replies, empty until the node is up */
static volatile bool running, done;

void serve_set_ip(const char *dotted) {
    char t[20];
    snprintf(t, sizeof t, "%s", dotted);
    for (char *c = t; *c; c++) if (*c == '.') *c = ',';
    LOCK_TCPIP_CORE();
    memcpy(tail_ip, t, sizeof t);
    UNLOCK_TCPIP_CORE();
}

/* ---- lwIP callbacks, tcpip_thread ---- */

static void on_err(void *arg, err_t err) {
    conn_t *c = arg;
    if (c) { c->pcb = NULL; c->dead = true; }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    if (!c) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { c->peer_fin = true; return ERR_OK; }
    if (c->rx) pbuf_cat(c->rx, p); else c->rx = p;
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK || !pcb) return ERR_VAL;
    for (int i = 0; i < CONNS; i++) {
        conn_t *c = &conns[i];
        if (c->state != FREE) continue;
        memset(c, 0, sizeof *c);
        c->state = NEW;
        c->pcb = pcb;
        c->port = (u16)(uintptr_t)arg;
        c->fd = -1;
        tcp_arg(pcb, c);
        tcp_recv(pcb, on_recv);
        tcp_err(pcb, on_err);
        return ERR_OK;
    }
    return ERR_MEM;   /* lwIP aborts the connection */
}

/* ---- bridge thread, core lock held ---- */

static bool again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY; }

static void dial(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) { c->dead = true; return; }
    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(c->port);
    a.sin_addr.s_addr = c->tries++ == 0 ? htonl(INADDR_LOOPBACK) : (u32)gethostid();
    c->t0 = os_ms();
    c->state = DIALING;
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) == 0) c->state = OPEN;
    else if (!again()) c->state = c->tries < 2 ? NEW : (c->dead = true, DIALING);
}

static void dialing(conn_t *c) {
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(c->port);
    a.sin_addr.s_addr = c->tries == 1 ? htonl(INADDR_LOOPBACK) : (u32)gethostid();
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) == 0 || errno == EISCONN) { c->state = OPEN; return; }
    bool waiting = again() && os_ms() - c->t0 < DIAL_MS;
    if (!waiting) { if (c->tries < 2) c->state = NEW; else c->dead = true; }
}

/* 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2): the four host fields become tail_ip. */
static size_t rewrite_227(u8 *b, size_t n, size_t cap) {
    if (!tail_ip[0] || n < 4 || memcmp(b, "227 ", 4)) return n;
    u8 *open = memchr(b, '(', n);
    if (!open) return n;
    u8 *p = open + 1, *end = b + n;
    for (int commas = 0; p < end && commas < 4; p++) if (*p == ',') commas++;
    if (p >= end) return n;
    size_t ip_len = strlen(tail_ip), head = open + 1 - b, tail = end - p;
    if (head + ip_len + 1 + tail > cap) return n;
    memmove(b + head + ip_len + 1, p, tail);
    memcpy(b + head, tail_ip, ip_len);
    b[head + ip_len] = ',';
    return head + ip_len + 1 + tail;
}

static void pump(conn_t *c) {
    /* tailnet -> soc */
    while (c->rx) {
        ssize_t n = send(c->fd, c->rx->payload, c->rx->len, 0);
        if (n < 0) { if (!again()) c->dead = true; break; }
        if (n == 0) break;
        c->rx = pbuf_free_header(c->rx, (u16)n);
        if (c->pcb) tcp_recved(c->pcb, (u16)n);
    }
    if (!c->rx && c->peer_fin && !c->fd_shut) { shutdown(c->fd, SHUT_WR); c->fd_shut = true; }

    /* soc -> tailnet */
    if (!c->pcb || c->local_fin) return;
    if (!c->pend_len) {
        size_t room = tcp_sndbuf(c->pcb);
        if (room > BUF) room = BUF;
        if (room == 0) return;
        ssize_t n = recv(c->fd, c->pend, room, 0);
        if (n == 0) {
            c->local_fin = true;
            tcp_shutdown(c->pcb, 0, 1);
            return;
        }
        if (n < 0) { if (!again()) c->dead = true; return; }
        c->pend_len = c->port == ctrl_port ? rewrite_227(c->pend, (size_t)n, sizeof c->pend) : (size_t)n;
    }
    if (c->pend_len > tcp_sndbuf(c->pcb)) return;
    if (tcp_write(c->pcb, c->pend, (u16)c->pend_len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        c->pend_len = 0;
        tcp_output(c->pcb);
    }
}

static void finish(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
    }
    if (c->rx) pbuf_free(c->rx);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void bridge(void *arg) {
    while (running) {
        bool busy = false;
        LOCK_TCPIP_CORE();
        for (int i = 0; i < CONNS; i++) {
            conn_t *c = &conns[i];
            if (c->state == FREE) continue;
            busy = true;
            if (!c->dead && c->state == NEW) dial(c);
            if (!c->dead && c->state == DIALING) dialing(c);
            if (!c->dead && c->state == OPEN) pump(c);
            bool closed = c->state == OPEN && c->peer_fin && !c->rx && c->local_fin && !c->pend_len;
            if (c->dead || closed) finish(c);
        }
        UNLOCK_TCPIP_CORE();
        os_sleep_ms(busy ? BUSY_MS : IDLE_MS);
    }
    done = true;
}

int serve_start(u16 lo, u16 hi) {
    if (running) return 0;
    LOCK_TCPIP_CORE();
    for (u32 port = lo; port <= hi && nlisten < (int)(sizeof listeners / sizeof *listeners); port++) {
        struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!p) break;
        if (tcp_bind(p, IP4_ADDR_ANY, (u16)port) != ERR_OK) { tcp_close(p); continue; }
        struct tcp_pcb *l = tcp_listen(p);
        if (!l) { tcp_close(p); break; }
        tcp_arg(l, (void *)(uintptr_t)port);
        tcp_accept(l, on_accept);
        listeners[nlisten++] = l;
    }
    UNLOCK_TCPIP_CORE();
    for (int i = 0; i < CONNS; i++) conns[i].fd = -1;
    ctrl_port = lo;
    running = true;
    done = false;
    if (!os_thread_start(bridge, NULL, 16 * 1024, 5)) { running = false; return -1; }
    return nlisten;
}

void serve_stop(void) {
    if (!running) return;
    running = false;
    for (int i = 0; i < 200 && !done; i++) os_sleep_ms(5);
    LOCK_TCPIP_CORE();
    for (int i = 0; i < CONNS; i++) if (conns[i].state != FREE) finish(&conns[i]);
    for (int i = 0; i < nlisten; i++) tcp_close(listeners[i]);
    nlisten = 0;
    tail_ip[0] = '\0';
    UNLOCK_TCPIP_CORE();
}
