/* Reverse tunnel client. Relay dials the broker (relay/tunnel.py) over TLS and keeps that one
 * stream open; the broker multiplexes FTP control and PASV data connections over it, and this
 * thread bridges each to a local connection to ftp.c's own ports, so ftp.c is unchanged.
 * Wire format, auth and PASV rewriting: the docstring at the top of relay/tunnel.py.
 *
 * sdmc:/3ds/relay/tunnel, one "key value" per line; the file existing turns the tunnel on:
 *   host   broker name or address (Funnel: knave.<tailnet>.ts.net)
 *   port   default 10000, the broker's Funnel port
 *   secret shared with the broker, 16 bytes or more; never logged, never shown
 *   pin    optional, sha256 hex of the broker's certificate DER: replaces the CA and name check,
 *          for a self-signed broker; without it the chain must verify against certs/cacert.pem
 *
 * Constraints: one soc:U session per process, and a socket call that blocks holds it against
 * every other thread (FTP sessions, the Artic server). Every socket here is non-blocking, poll
 * takes a zero timeout, waiting is svcSleepThread. The one exception is getaddrinfo, once per
 * dial, as libcurl in net.cpp does; a dotted address skips it. Threads of equal priority on one
 * core never preempt each other, so the loop sleeps every pass, 1 ms busy, 10 ms quiet.
 *
 * Keepalive: PING after PING_IDLE s without sending, tunnel dropped after DEAD s without a frame.
 * Redial: backoff 1 s doubling to 60 s, back to 1 s after a tunnel that stayed up 30 s. */
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "cacert_pem.h"
#include "ftp.h"
#include "tunnel.h"

#define CONF_PATH  "sdmc:/3ds/relay/tunnel"
#define STREAMS    8
#define MAXP       16384
#define HDR        5
#define RXCAP      (2 * (MAXP + HDR))
#define TXCAP      (2 * (MAXP + HDR))
#define PING_IDLE  25000
#define DEAD       75000
#define DIAL_MS    10000
#define HELLO_MS   20000
#define PORT_LO    FTP_PORT
#define PORT_HI    (FTP_PORT + 100)   /* ftp.c's PASV range, PASV_FIRST + PASV_COUNT - 1 */
#define STACK_SIZE 0x10000

enum { CHAL = 1, AUTH, OPEN, DATA, FIN, PING, PONG };
static const char TAG[] = "relay-tunnel 1";

typedef struct {
    bool used, connecting, fin_rx, fin_tx, shut;
    int fd, port, tries;
    u16 id;
    u8 *out;            /* bytes from the broker the local socket has not taken yet */
    size_t out_len;
} stream_t;

static struct {
    char host[128], secret[128];
    int port;
    bool pinned;
    u8 pin[32];
} conf;

static Thread thread;
static volatile bool running;
static LightLock lock;
static int state = TUNNEL_OFF;
static char state_line[48];
static stream_t st[STREAMS];
static u8 rx[RXCAP], tx[TXCAP];
static size_t rx_len, tx_len, tx_pending;   /* tx_pending: length of an ssl_write that said WANT_WRITE */

static void set_state(int s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(int s, const char *fmt, ...) {
    LightLock_Lock(&lock);
    state = s;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state_line, sizeof(state_line), fmt, ap);
    va_end(ap);
    LightLock_Unlock(&lock);
}

int tunnel_status(char *out, size_t cap) {
    if (!thread) { if (cap) out[0] = '\0'; return TUNNEL_OFF; }   /* lock is set up by tunnel_start */
    LightLock_Lock(&lock);
    int s = state;
    snprintf(out, cap, "%s", state_line);
    LightLock_Unlock(&lock);
    return s;
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool load_conf(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return false;
    memset(&conf, 0, sizeof(conf));
    conf.port = 10000;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *v = strchr(line, ' ');
        if (!v) continue;
        *v++ = '\0';
        while (*v == ' ') v++;
        if (!strcmp(line, "host")) snprintf(conf.host, sizeof(conf.host), "%s", v);
        else if (!strcmp(line, "port")) conf.port = atoi(v);
        else if (!strcmp(line, "secret")) snprintf(conf.secret, sizeof(conf.secret), "%s", v);
        else if (!strcmp(line, "pin") && strlen(v) == 64) {
            conf.pinned = true;
            for (int i = 0; i < 32; i++) {
                int hi = hexval(v[2 * i]), lo = hexval(v[2 * i + 1]);
                if (hi < 0 || lo < 0) conf.pinned = false;
                conf.pin[i] = (u8)(hi << 4 | lo);
            }
        }
    }
    fclose(f);
    memset(line, 0, sizeof(line));
    if (!conf.host[0] || strlen(conf.secret) < 16 || conf.port <= 0 || conf.port > 65535) {
        ftp_log("tunnel: %s needs host and a secret of 16+ bytes", CONF_PATH);
        return false;
    }
    return true;
}

/* mbedTLS over a non-blocking fd. */
static int bio_send(void *ctx, const unsigned char *p, size_t n) {
    ssize_t w = send(*(int *)ctx, p, n, 0);
    if (w < 0) return would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)w;
}
static int bio_recv(void *ctx, unsigned char *p, size_t n) {
    ssize_t r = recv(*(int *)ctx, p, n, 0);
    if (r < 0) return would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)r;
}

/* Non-blocking connect, finished by polling for writable. -1 on failure. */
static int dial(u32 ip, int port, int ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    nonblock(fd);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
    if (errno != EINPROGRESS && !would_block()) { close(fd); return -1; }
    for (; running && ms > 0; ms -= 10) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int r = poll(&p, 1, 0);
        if (r < 0 || (r > 0 && (p.revents & (POLLERR | POLLHUP)))) break;
        if (r > 0) return fd;
        svcSleepThread(10000000LL);
    }
    close(fd);
    return -1;
}

static u32 resolve(const char *host) {
    struct in_addr a;
    if (inet_aton(host, &a)) return a.s_addr;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    u32 ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return ip;
}

/* TLS writer: queues a frame; false when tx has no room (caller keeps its data for later). */
static bool frame(int type, u16 id, const void *p, size_t n) {
    if (tx_len + HDR + n > TXCAP) return false;
    u8 *h = tx + tx_len;
    h[0] = type; h[1] = id >> 8; h[2] = id; h[3] = n >> 8; h[4] = n;
    if (n) memcpy(h + HDR, p, n);
    tx_len += HDR + n;
    return true;
}

/* -1 fatal, else bytes moved. */
static int flush_tx(mbedtls_ssl_context *ssl) {
    if (!tx_len) return 0;
    size_t n = tx_pending ? tx_pending : (tx_len > MAXP ? MAXP : tx_len);
    int w = mbedtls_ssl_write(ssl, tx, n);
    if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) { tx_pending = n; return 0; }
    tx_pending = 0;
    if (w < 0) return -1;
    memmove(tx, tx + w, tx_len - w);
    tx_len -= w;
    return w;
}

static stream_t *find(u16 id) {
    for (int i = 0; i < STREAMS; i++) if (st[i].used && st[i].id == id) return &st[i];
    return NULL;
}

static void stream_free(stream_t *s) {
    if (s->fd >= 0) close(s->fd);
    free(s->out);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

static void streams_reset(void) {
    for (int i = 0; i < STREAMS; i++) if (st[i].used) stream_free(&st[i]);
    for (int i = 0; i < STREAMS; i++) st[i].fd = -1;
}

/* Starts a local connect for an OPEN; 127.0.0.1 first, then the console's own address. */
static void local_dial(stream_t *s) {
    u32 ip = s->tries++ == 0 ? htonl(INADDR_LOOPBACK) : (u32)gethostid();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        nonblock(fd);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ip;
        a.sin_port = htons(s->port);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 || errno == EINPROGRESS || would_block()) {
            s->fd = fd;
            s->connecting = true;
            return;
        }
        close(fd);
    }
    s->fd = -1;
}

/* A local connect that failed: next address, or FIN to the broker once both are spent. */
static void local_failed(stream_t *s) {
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->connecting = false;
    if (s->tries < 2) local_dial(s);
    if (s->fd < 0) {
        s->fin_rx = true;   /* nothing more will be written here */
        if (!s->fin_tx && frame(FIN, s->id, NULL, 0)) s->fin_tx = true;
    }
}

/* The local socket failed (ftp.c closed it, often with a reset): drop it and what was queued for
 * it, FIN to the broker. Later DATA for the stream is discarded, so the tunnel never waits on it. */
static void local_dead(stream_t *s, int err) {
    if (err != ECONNRESET && err != EPIPE) ftp_log("tunnel: local port %d errno %d", s->port, err);
    close(s->fd);
    s->fd = -1;
    s->out_len = 0;
    s->fin_rx = true;
    if (!s->fin_tx && frame(FIN, s->id, NULL, 0)) s->fin_tx = true;
}

/* Writes as much of s->out as the local socket takes. -1 when the local side is gone. */
static int drain(stream_t *s) {
    while (s->out_len && s->fd >= 0 && !s->connecting) {
        ssize_t w = send(s->fd, s->out, s->out_len, 0);
        if (w < 0) {
            if (would_block()) return 0;
            local_dead(s, errno);
            return -1;
        }
        memmove(s->out, s->out + w, s->out_len - w);
        s->out_len -= w;
    }
    if (!s->out_len && s->fin_rx && !s->shut && s->fd >= 0 && !s->connecting) {
        shutdown(s->fd, SHUT_WR);
        s->shut = true;
    }
    return 0;
}

/* One frame from the broker. 1 consumed, 0 must wait (stream busy, tx full), -1 protocol fault. */
static int handle(int type, u16 id, const u8 *p, size_t n) {
    if (type == PING) return frame(PONG, 0, p, n) ? 1 : 0;
    if (type == PONG) return 1;
    stream_t *s = find(id);
    if (type == OPEN) {
        if (s || n != 2) return -1;
        int port = p[0] << 8 | p[1];
        for (int i = 0; i < STREAMS && !s; i++) if (!st[i].used) s = &st[i];
        u8 *out = s ? malloc(MAXP) : NULL;
        if (!out || port < PORT_LO || port > PORT_HI) {
            free(out);
            if (!frame(FIN, id, NULL, 0)) return 0;
            ftp_log("tunnel: refused open of port %d", port);
            return 1;   /* the broker's FIN for it lands on no stream and is dropped */
        }
        memset(s, 0, sizeof(*s));
        s->used = true; s->id = id; s->port = port; s->out = out; s->fd = -1;
        local_dial(s);
        if (s->fd < 0) local_failed(s);
        return 1;
    }
    if (!s) return 1;
    if (type == DATA) {
        if (n > MAXP) return -1;
        if (s->out_len) return 0;
        if (s->fin_rx) return 1;   /* local side refused or gone: drop it */
        memcpy(s->out, p, n);
        s->out_len = n;
        drain(s);
        return 1;
    }
    if (type == FIN) {
        s->fin_rx = true;
        drain(s);
        return 1;
    }
    return -1;
}

/* One pass over local sockets: connects finishing, reads into DATA frames, EOF into FIN. */
static bool pump_local(void) {
    struct pollfd p[STREAMS];
    int map[STREAMS], np = 0;
    bool moved = false;
    for (int i = 0; i < STREAMS; i++) {
        stream_t *s = &st[i];
        if (!s->used || s->fd < 0) continue;
        if (!s->connecting && s->fin_tx && !s->out_len) continue;
        p[np].fd = s->fd;
        p[np].events = s->connecting ? POLLOUT : (s->fin_tx ? 0 : POLLIN) | (s->out_len ? POLLOUT : 0);
        p[np].revents = 0;
        map[np++] = i;
    }
    if (np && poll(p, np, 0) > 0) {
        for (int k = 0; k < np; k++) {
            stream_t *s = &st[map[k]];
            short ev = p[k].revents;
            if (!ev) continue;
            moved = true;
            if (s->connecting) {
                if (ev & (POLLERR | POLLHUP)) local_failed(s);
                else { s->connecting = false; drain(s); }
                continue;
            }
            if (s->out_len && (ev & (POLLOUT | POLLERR | POLLHUP)) && drain(s) < 0) continue;
            if (!s->fin_tx && (ev & (POLLIN | POLLERR | POLLHUP)) && TXCAP - tx_len >= HDR + MAXP) {
                ssize_t r = recv(s->fd, tx + tx_len + HDR, MAXP, 0);
                if (r > 0) {
                    u8 *h = tx + tx_len;
                    h[0] = DATA; h[1] = s->id >> 8; h[2] = s->id; h[3] = r >> 8; h[4] = r;
                    tx_len += HDR + r;
                } else if (r == 0) {
                    if (frame(FIN, s->id, NULL, 0)) s->fin_tx = true;
                } else if (!would_block()) {
                    local_dead(s, errno);
                }
            }
        }
    }
    for (int i = 0; i < STREAMS; i++) {
        stream_t *s = &st[i];
        /* A FIN that found tx full is sent on a later pass. */
        if (s->used && s->fd < 0 && !s->fin_tx && frame(FIN, s->id, NULL, 0)) s->fin_tx = true;
        if (s->used && s->fin_rx && s->fin_tx && !s->out_len) stream_free(s);
    }
    return moved;
}

/* One tunnel from dial to drop. Returns how long it stayed up, ms (0 = never authenticated). */
static u64 session(mbedtls_ssl_config *cfg) {
    set_state(TUNNEL_DIALING, "Tunnel dialing");
    u32 ip = resolve(conf.host);
    if (!ip) { set_state(TUNNEL_WAIT, "Tunnel: no address"); ftp_log("tunnel: cannot resolve %s", conf.host); return 0; }
    int fd = dial(ip, conf.port, DIAL_MS);
    if (fd < 0) { ftp_log("tunnel: cannot reach %s:%d", conf.host, conf.port); return 0; }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    u64 up = 0, t0 = osGetTime();
    int r;
    if (mbedtls_ssl_setup(&ssl, cfg) != 0 || mbedtls_ssl_set_hostname(&ssl, conf.host) != 0) goto out;
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || !running ||
            osGetTime() - t0 > HELLO_MS) {
            char e[80];
            mbedtls_strerror(r, e, sizeof(e));
            ftp_log("tunnel: tls %s", e);
            goto out;
        }
        svcSleepThread(5000000LL);
    }
    if (conf.pinned) {
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
        u8 h[32];
        if (!peer || mbedtls_sha256_ret(peer->raw.p, peer->raw.len, h, 0) != 0 || memcmp(h, conf.pin, 32)) {
            ftp_log("tunnel: broker certificate does not match pin");
            goto out;
        }
    }

    rx_len = tx_len = tx_pending = 0;
    streams_reset();
    /* CHAL, then AUTH = HMAC-SHA256(secret, nonce + TAG). */
    while (rx_len < HDR + 32) {
        r = mbedtls_ssl_read(&ssl, rx + rx_len, HDR + 32 - rx_len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!running || osGetTime() - t0 > HELLO_MS) goto out;
            svcSleepThread(5000000LL);
            continue;
        }
        if (r <= 0) goto out;
        rx_len += r;
    }
    if (rx[0] != CHAL || (rx[3] << 8 | rx[4]) != 32) { ftp_log("tunnel: bad hello"); goto out; }
    {
        u8 msg[32 + sizeof(TAG) - 1], mac[32];
        memcpy(msg, rx + HDR, 32);
        memcpy(msg + 32, TAG, sizeof(TAG) - 1);
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const u8 *)conf.secret, strlen(conf.secret),
                        msg, sizeof(msg), mac);
        frame(AUTH, 0, mac, 32);
        memset(mac, 0, sizeof(mac));
    }
    rx_len = 0;

    u64 start = osGetTime(), last_rx = start, last_tx = start;
    int quiet = 0;
    ftp_log("tunnel up to %s", conf.host);
    set_state(TUNNEL_UP, "Tunnel up");
    while (running) {
        bool moved = false;
        u64 now = osGetTime();
        if (now - last_tx > PING_IDLE && frame(PING, 0, NULL, 0)) last_tx = now;
        if (now - last_rx > DEAD) { ftp_log("tunnel: broker silent, redialing"); break; }

        /* Broker to console: parse whole frames; a busy stream holds the rest back. */
        bool blocked = false;
        size_t off = 0;
        while (rx_len - off >= HDR) {
            const u8 *h = rx + off;
            size_t n = h[3] << 8 | h[4];
            if (n > MAXP) goto drop;
            if (rx_len - off < HDR + n) break;
            int k = handle(h[0], h[1] << 8 | h[2], h + HDR, n);
            if (k < 0) { ftp_log("tunnel: protocol fault"); goto drop; }
            if (k == 0) { blocked = true; break; }
            off += HDR + n;
            moved = true;
        }
        if (off) { memmove(rx, rx + off, rx_len - off); rx_len -= off; }
        if (!blocked && rx_len < RXCAP) {
            r = mbedtls_ssl_read(&ssl, rx + rx_len, RXCAP - rx_len);
            if (r > 0) { rx_len += r; last_rx = now; moved = true; }
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) { ftp_log("tunnel: broker closed"); break; }
        }
        for (int i = 0; i < STREAMS; i++) if (st[i].used && st[i].out_len) drain(&st[i]);

        if (pump_local()) moved = true;
        r = flush_tx(&ssl);
        if (r < 0) { ftp_log("tunnel: send failed"); break; }
        if (r > 0) { last_tx = now; moved = true; }

        quiet = moved ? 0 : quiet + 1;
        svcSleepThread(quiet > 50 ? 10000000LL : 1000000LL);
    }
drop:
    up = osGetTime() - start;
    mbedtls_ssl_close_notify(&ssl);
    streams_reset();
out:
    mbedtls_ssl_free(&ssl);
    close(fd);
    return up ? up : 0;
}

static void loop(void *unused) {
    (void)unused;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config cfg;
    mbedtls_x509_crt ca;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_config_init(&cfg);
    mbedtls_x509_crt_init(&ca);
    bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const u8 *)TAG, sizeof(TAG) - 1) == 0 &&
              mbedtls_ssl_config_defaults(&cfg, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ok) {
        mbedtls_ssl_conf_rng(&cfg, mbedtls_ctr_drbg_random, &drbg);
        /* ChaCha20 first: the ARM11 has no AES unit. A server that honours client order (Go's, under
         * Funnel) picks it. */
        static const int suites[] = {
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0,
        };
        mbedtls_ssl_conf_ciphersuites(&cfg, suites);
        if (conf.pinned) mbedtls_ssl_conf_authmode(&cfg, MBEDTLS_SSL_VERIFY_NONE);
        else {
            /* x509 PEM parsing wants the terminating NUL bin2s does not add. */
            char *pem = malloc(cacert_pem_size + 1);
            ok = pem != NULL;
            if (ok) {
                memcpy(pem, cacert_pem, cacert_pem_size);
                pem[cacert_pem_size] = '\0';
                ok = mbedtls_x509_crt_parse(&ca, (const u8 *)pem, cacert_pem_size + 1) >= 0;
                free(pem);
            }
            mbedtls_ssl_conf_authmode(&cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&cfg, &ca, NULL);
        }
    }
    if (!ok) { set_state(TUNNEL_WAIT, "Tunnel: TLS setup failed"); ftp_log("tunnel: tls setup failed"); }

    int backoff = 1;
    while (ok && running) {
        if (!gethostid()) {
            set_state(TUNNEL_WAIT, "Tunnel waits for Wi-Fi");
            svcSleepThread(1000000000LL);
            continue;
        }
        u64 up = session(&cfg);
        if (!running) break;
        if (up >= 30000) backoff = 1;
        for (int left = backoff; running && left > 0; left--) {
            set_state(TUNNEL_WAIT, "Tunnel retry %d s", left);
            svcSleepThread(1000000000LL);
        }
        backoff = backoff * 2 > 60 ? 60 : backoff * 2;
    }
    mbedtls_x509_crt_free(&ca);
    mbedtls_ssl_config_free(&cfg);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
}

void tunnel_start(int prio) {
    LightLock_Init(&lock);
    set_state(TUNNEL_OFF, "%s", "");
    if (thread || !load_conf()) return;
    running = true;
    set_state(TUNNEL_DIALING, "Tunnel dialing");
    thread = threadCreate(loop, NULL, STACK_SIZE, prio, -2, false);
    if (!thread) { running = false; set_state(TUNNEL_OFF, "%s", ""); }
}

void tunnel_stop(void) {
    if (!thread) return;
    running = false;
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    thread = NULL;
    memset(conf.secret, 0, sizeof(conf.secret));
    set_state(TUNNEL_OFF, "%s", "");
}
