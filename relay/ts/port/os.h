/* The one OS layer under the FreeRTOS, ESP-IDF and lwIP shims. Two backends:
 * os_posix.c (Mac host build) and os_ctru.c (3DS, libctru). Nothing above this file
 * calls pthreads or svc directly. */
#ifndef TS_OS_H
#define TS_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os_thread os_thread_t;
typedef struct os_mutex os_mutex_t;
typedef struct os_cond os_cond_t;

/* Once, before any other call here. 0 on success. */
int os_init(void);

/* Monotonic milliseconds and microseconds since an arbitrary start. */
uint64_t os_ms(void);
uint64_t os_us(void);
void os_sleep_ms(uint32_t ms);

/* prio: FreeRTOS-style, higher runs first. Backends map it onto their own range.
 * The thread frees its own handle when fn returns. */
os_thread_t *os_thread_start(void (*fn)(void *), void *arg, size_t stack, int prio);
/* Ends the calling thread; its handle is freed as if fn had returned. */
void os_thread_exit(void) __attribute__((noreturn));

os_mutex_t *os_mutex_new(bool recursive);
void os_mutex_free(os_mutex_t *m);
void os_mutex_lock(os_mutex_t *m);
void os_mutex_unlock(os_mutex_t *m);

os_cond_t *os_cond_new(void);
void os_cond_free(os_cond_t *c);
/* Waits on c with m held. timeout_ms UINT32_MAX = forever. Returns false on timeout.
 * Spurious wakeups are possible; callers loop on their predicate. */
bool os_cond_wait(os_cond_t *c, os_mutex_t *m, uint32_t timeout_ms);
void os_cond_broadcast(os_cond_t *c);

/* Cryptographic random bytes. */
void os_random(void *buf, size_t len);

/* One log line, already formatted, no trailing newline. Goes to the sink set by os_set_log:
 * none on the console until the embedder sets one, stderr on the host. */
void os_log_line(const char *line);
void os_set_log(void (*fn)(const char *line));

#endif
