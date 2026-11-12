#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FTP_PORT
#define FTP_PORT 5000
#endif

/* Starts the listener thread. socInit must already have run. prio: thread priority, larger = lower.
 * banner: 220 greeting text; Azahar's launcher tells app from plugin by it. */
int ftp_start(int prio, const char *banner);
/* Stops the listener and every session, then joins them. */
void ftp_stop(void);
/* Open sessions. */
int ftp_sessions(void);

/* Snapshot for a status screen. last may tear for one frame while a session writes it. */
typedef struct {
    int sessions;
    unsigned long long rx, tx;  /* bytes received, sent; data connections only */
    char last[96];              /* most recent file action, "put /3ds/x" */
} ftp_status_t;
void ftp_status(ftp_status_t *out);

/* Host extensions, called from session threads. Any member may be NULL. */
typedef struct {
    /* SITE <arg>, arg holding the verb and its operands. Writes the reply text to out ('\n' splits
     * lines) and returns the FTP code; 0 = unknown verb, answered 500. */
    int (*site)(char *arg, char *out, size_t cap);
    /* STOR redirect: 1 = take the upload (ctx set), 0 = plain file, -1 = refuse (err set). */
    int (*sink_open)(const char *abs, void **ctx, const char **err);
    int (*sink_write)(void *ctx, const void *p, size_t n);   /* 0 = failed */
    const char *(*sink_close)(void *ctx, int ok);           /* NULL = done, else why it failed */
} ftp_hooks_t;
/* Set before ftp_start; the struct must outlive the server. */
void ftp_set_hooks(const ftp_hooks_t *hooks);

/* Event log: a ring of the last FTP_LOG_LINES lines, read back by SITE LOG. */
#define FTP_LOG_LINES 40
void ftp_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
