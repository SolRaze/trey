/* microlink's underlay sockets are the OS's own: this shadows lwIP's socket header.
 * On the console the blocking calls go through os_ctru.c's ts_* wrappers (the soc rule there);
 * every system header they touch is included first, so the macros never rename a declaration. */
#ifndef TS_LWIP_SOCKETS_H
#define TS_LWIP_SOCKETS_H
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE   /* macOS name */
#endif

#ifdef __3DS__
/* soc has none of these; ts_setsockopt keeps the timeouts and drops the keepalive timing */
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 0x0008
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE  0x4
#define TCP_KEEPINTVL 0x5
#define TCP_KEEPCNT   0x6
#endif

/* soc is IPv4 only: AF_INET6 sockets fail at socket(), these let the IPv6 paths compile */
#ifndef IN6ADDR_ANY_INIT
struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};
#define IN6ADDR_ANY_INIT {{ 0 }}
#endif

int ts_socket(int dom, int type, int proto);
int ts_close(int fd);
int ts_fcntl(int fd, int cmd, ...);
int ts_setsockopt(int fd, int level, int name, const void *v, socklen_t len);
int ts_connect(int fd, const struct sockaddr *a, socklen_t len);
ssize_t ts_recv(int fd, void *b, size_t n, int fl);
ssize_t ts_recvfrom(int fd, void *b, size_t n, int fl, struct sockaddr *a, socklen_t *al);
ssize_t ts_send(int fd, const void *b, size_t n, int fl);
ssize_t ts_sendto(int fd, const void *b, size_t n, int fl, const struct sockaddr *a, socklen_t al);
ssize_t ts_read(int fd, void *b, size_t n);
ssize_t ts_write(int fd, const void *b, size_t n);
int ts_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
int ts_getaddrinfo(const char *host, const char *svc, const struct addrinfo *hints, struct addrinfo **res);

#define socket      ts_socket
#define close       ts_close
#define fcntl       ts_fcntl
#define setsockopt  ts_setsockopt
#define connect     ts_connect
#define recv        ts_recv
#define recvfrom    ts_recvfrom
#define send        ts_send
#define sendto      ts_sendto
#define read        ts_read
#define write       ts_write
#define select      ts_select
#define getaddrinfo ts_getaddrinfo
#endif
#endif
