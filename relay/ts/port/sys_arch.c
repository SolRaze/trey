/* lwIP sys layer over the FreeRTOS shim, plus the core lock with holder tracking that
 * ESP-lwIP's sys_thread_tcpip() reports (lwipopts.h routes LOCK_TCPIP_CORE here). */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "os.h"

#include <string.h>

static _Thread_local int core_depth;      /* core lock held by this thread */
static _Thread_local int is_tcpip;        /* this thread is lwIP's tcpip thread */
static os_mutex_t *core;
static os_mutex_t *prot;

void sys_init(void) {
    if (!core) core = os_mutex_new(true);
    if (!prot) prot = os_mutex_new(true);
}

u32_t sys_now(void) { return (u32_t)os_ms(); }

void ts_lock_core(void) { os_mutex_lock(core); core_depth++; }
void ts_unlock_core(void) { core_depth--; os_mutex_unlock(core); }

int sys_thread_tcpip(int query) {
    (void)query;
    return is_tcpip || core_depth > 0;
}

sys_prot_t sys_arch_protect(void) { os_mutex_lock(prot); return 0; }
void sys_arch_unprotect(sys_prot_t v) { (void)v; os_mutex_unlock(prot); }

/* semaphores */

err_t sys_sem_new(sys_sem_t *s, u8_t count) {
    *s = xSemaphoreCreateCounting(0xFFFF, count);
    return *s ? ERR_OK : ERR_MEM;
}

void sys_sem_free(sys_sem_t *s) { vSemaphoreDelete(*s); *s = NULL; }
void sys_sem_signal(sys_sem_t *s) { xSemaphoreGive(*s); }

u32_t sys_arch_sem_wait(sys_sem_t *s, u32_t timeout) {
    u32_t start = sys_now();
    if (xSemaphoreTake(*s, timeout ? timeout : portMAX_DELAY) != pdPASS) return SYS_ARCH_TIMEOUT;
    return sys_now() - start;
}

/* mutexes */

err_t sys_mutex_new(sys_mutex_t *m) {
    *m = xSemaphoreCreateMutex();
    return *m ? ERR_OK : ERR_MEM;
}

void sys_mutex_free(sys_mutex_t *m) { vSemaphoreDelete(*m); *m = NULL; }
void sys_mutex_lock(sys_mutex_t *m) { xSemaphoreTake(*m, portMAX_DELAY); }
void sys_mutex_unlock(sys_mutex_t *m) { xSemaphoreGive(*m); }

/* mailboxes: queues of pointers */

err_t sys_mbox_new(sys_mbox_t *mb, int size) {
    *mb = xQueueCreate(size > 0 ? (UBaseType_t)size : 16, sizeof(void *));
    return *mb ? ERR_OK : ERR_MEM;
}

void sys_mbox_free(sys_mbox_t *mb) { vQueueDelete(*mb); *mb = NULL; }
void sys_mbox_post(sys_mbox_t *mb, void *msg) { xQueueSend(*mb, &msg, portMAX_DELAY); }
err_t sys_mbox_trypost(sys_mbox_t *mb, void *msg) { return xQueueSend(*mb, &msg, 0) == pdPASS ? ERR_OK : ERR_MEM; }
err_t sys_mbox_trypost_fromisr(sys_mbox_t *mb, void *msg) { return sys_mbox_trypost(mb, msg); }

u32_t sys_arch_mbox_fetch(sys_mbox_t *mb, void **msg, u32_t timeout) {
    void *dummy;
    u32_t start = sys_now();
    if (xQueueReceive(*mb, msg ? msg : &dummy, timeout ? timeout : portMAX_DELAY) != pdPASS) {
        if (msg) *msg = NULL;
        return SYS_ARCH_TIMEOUT;
    }
    return sys_now() - start;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mb, void **msg) {
    void *dummy;
    return xQueueReceive(*mb, msg ? msg : &dummy, 0) == pdPASS ? 0 : SYS_MBOX_EMPTY;
}

/* threads */

struct start { lwip_thread_fn fn; void *arg; int tcpip; };

static void thread_entry(void *p) {
    struct start s = *(struct start *)p;
    mem_free(p);
    is_tcpip = s.tcpip;
    s.fn(s.arg);
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn fn, void *arg, int stack, int prio) {
    struct start *s = mem_malloc(sizeof *s);
    if (!s) return NULL;
    s->fn = fn;
    s->arg = arg;
    s->tcpip = name && strcmp(name, TCPIP_THREAD_NAME) == 0;
    TaskHandle_t t = NULL;
    if (xTaskCreate(thread_entry, name, (uint32_t)stack, s, (UBaseType_t)prio, &t) != pdPASS) {
        mem_free(s);
        return NULL;
    }
    return t;
}
