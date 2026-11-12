/* Host stand-ins for the libctru calls ftp.c makes, so it runs on the Mac under test/host.sh. */
#pragma once
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int32_t Result;
typedef pthread_t *Thread;
#define U64_MAX UINT64_MAX

static inline Thread threadCreate(void (*fn)(void *), void *arg, size_t stack, int prio, int core, bool detached) {
    (void)stack; (void)prio; (void)core;
    Thread t = malloc(sizeof(pthread_t));
    if (pthread_create(t, NULL, (void *(*)(void *))fn, arg)) { free(t); return NULL; }
    if (detached) { pthread_detach(*t); free(t); return (Thread)1; }
    return t;
}
static inline void threadJoin(Thread t, u64 timeout) { (void)timeout; pthread_join(*t, NULL); }
static inline void threadFree(Thread t) { free(t); }
static inline void svcSleepThread(long long ns) {
    struct timespec ts = { ns / 1000000000LL, ns % 1000000000LL };
    nanosleep(&ts, NULL);
}
static inline Result archive_getmtime(const char *name, u64 *mt) {
    struct stat st;
    if (stat(name, &st)) return -1;
    *mt = st.st_mtime;
    return 0;
}
static inline long host_gethostid(void) { return (long)inet_addr("127.0.0.1"); }
#define gethostid host_gethostid
