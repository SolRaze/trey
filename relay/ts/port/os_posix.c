/* os.h on pthreads, for the Mac host build. */
#include "os.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct os_mutex { pthread_mutex_t m; };
struct os_cond { pthread_cond_t c; };
struct os_thread { void (*fn)(void *); void *arg; pthread_t t; };

uint64_t os_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t os_ms(void) { return os_us() / 1000u; }

void os_sleep_ms(uint32_t ms) {
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static _Thread_local struct os_thread *self;

static void *trampoline(void *p) {
    self = p;
    self->fn(self->arg);
    os_thread_exit();
}

void os_thread_exit(void) {
    free(self);
    self = NULL;
    pthread_exit(NULL);
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg, size_t stack, int prio) {
    (void)prio;
    struct os_thread *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    /* ESP stacks are sized for a 32-bit core; a 64-bit host needs more. */
    size_t sz = stack * 4 < 256 * 1024 ? 256 * 1024 : stack * 4;
    pthread_attr_setstacksize(&a, sz);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t->t, &a, trampoline, t);
    pthread_attr_destroy(&a);
    if (r) { free(t); return NULL; }
    return t;
}

os_mutex_t *os_mutex_new(bool recursive) {
    struct os_mutex *m = malloc(sizeof *m);
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (recursive) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

void os_mutex_free(os_mutex_t *m) { if (m) { pthread_mutex_destroy(&m->m); free(m); } }
void os_mutex_lock(os_mutex_t *m) { pthread_mutex_lock(&m->m); }
void os_mutex_unlock(os_mutex_t *m) { pthread_mutex_unlock(&m->m); }

os_cond_t *os_cond_new(void) {
    struct os_cond *c = malloc(sizeof *c);
    if (c) pthread_cond_init(&c->c, NULL);
    return c;
}

void os_cond_free(os_cond_t *c) { if (c) { pthread_cond_destroy(&c->c); free(c); } }
void os_cond_broadcast(os_cond_t *c) { pthread_cond_broadcast(&c->c); }

bool os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms) {
    if (timeout_ms == UINT32_MAX) { pthread_cond_wait(&c->c, &m->m); return true; }
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t ns = (uint64_t)now.tv_usec * 1000u + (uint64_t)timeout_ms * 1000000u;
    struct timespec until = { now.tv_sec + (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    return pthread_cond_timedwait(&c->c, &m->m, &until) != ETIMEDOUT;
}

void os_random(void *buf, size_t len) { arc4random_buf(buf, len); }

int os_init(void) { return 0; }

static void log_stderr(const char *line) {
    fprintf(stderr, "%llu %s\n", (unsigned long long)os_ms(), line);
}

static void (*log_fn)(const char *) = log_stderr;
void os_set_log(void (*fn)(const char *line)) { log_fn = fn; }
void os_log_line(const char *line) { if (log_fn) log_fn(line); }
