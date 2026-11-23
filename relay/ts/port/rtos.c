/* FreeRTOS shim (include/freertos/FreeRTOS.h) over os.h. One mutex + condition per object;
 * waits are deadline loops, so spurious wakeups and broadcasts are harmless. */
#include "freertos/FreeRTOS.h"
#include "os.h"

#include <stdlib.h>
#include <string.h>

/* ---- waiting ---- */

/* Milliseconds left until deadline, UINT32_MAX for forever, 0 when passed. */
static uint32_t left(uint64_t deadline) {
    if (deadline == UINT64_MAX) return UINT32_MAX;
    uint64_t now = os_ms();
    return now >= deadline ? 0 : (uint32_t)(deadline - now);
}

static uint64_t deadline_of(TickType_t wait) {
    return wait == portMAX_DELAY ? UINT64_MAX : os_ms() + wait;
}

/* ---- tasks ---- */

struct ts_task {
    TaskFunction_t fn;
    void *arg;
    os_mutex_t *lock;
    os_cond_t *cond;
    uint32_t notify;
};

static _Thread_local struct ts_task *current;

static void task_entry(void *p) {
    current = p;
    current->fn(current->arg);
    vTaskDelete(NULL);
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core) {
    (void)name; (void)core;
    struct ts_task *t = calloc(1, sizeof *t);
    if (!t) return pdFAIL;
    t->fn = fn;
    t->arg = arg;
    t->lock = os_mutex_new(false);
    t->cond = os_cond_new();
    if (out) *out = t;
    if (!t->lock || !t->cond || !os_thread_start(task_entry, t, stack, (int)prio)) {
        if (out) *out = NULL;
        os_mutex_free(t->lock);
        os_cond_free(t->cond);
        free(t);
        return pdFAIL;
    }
    return pdPASS;
}

/* The task record stays allocated: another task may still hold the handle and notify it.
 * microlink creates a fixed handful of tasks per start, so the leak is bounded. */
void vTaskDelete(TaskHandle_t t) {
    if (t && t != current) abort();     /* microlink tasks only ever end themselves */
    os_thread_exit();
}

void vTaskDelay(TickType_t ticks) { os_sleep_ms(ticks ? ticks : 1); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)os_ms(); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 0; }

BaseType_t xTaskNotifyGive(TaskHandle_t t) {
    if (!t) return pdFAIL;
    os_mutex_lock(t->lock);
    t->notify++;
    os_cond_broadcast(t->cond);
    os_mutex_unlock(t->lock);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    struct ts_task *t = current;
    if (!t) { vTaskDelay(wait == portMAX_DELAY ? 1 : wait); return 0; }
    uint64_t end = deadline_of(wait);
    os_mutex_lock(t->lock);
    while (!t->notify) {
        uint32_t ms = left(end);
        if (!ms) break;
        os_cond_wait(t->cond, t->lock, ms);
    }
    uint32_t v = t->notify;
    if (v) t->notify = clear ? 0 : v - 1;
    os_mutex_unlock(t->lock);
    return v;
}

/* ---- queues and semaphores ---- */

enum { Q_QUEUE, Q_SEM, Q_MUTEX, Q_RMUTEX };

struct ts_queue {
    int kind;
    os_mutex_t *lock;
    os_cond_t *cond;
    /* queue: ring of len items of size bytes | semaphore: count of max */
    uint8_t *buf;
    UBaseType_t len, size, head, count;
    /* mutexes */
    struct ts_task *owner;
    uintptr_t owner_id;
    UBaseType_t depth;
};

static struct ts_queue *q_new(int kind, UBaseType_t len, UBaseType_t size, UBaseType_t count) {
    struct ts_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->kind = kind;
    q->len = len;
    q->size = size;
    q->count = count;
    q->lock = os_mutex_new(false);
    q->cond = os_cond_new();
    if (size && len) q->buf = malloc((size_t)len * size);
    if (!q->lock || !q->cond || (size && len && !q->buf)) { vQueueDelete(q); return NULL; }
    return q;
}

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size) { return q_new(Q_QUEUE, len, size, 0); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return q_new(Q_SEM, 1, 0, 0); }
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t n) { return q_new(Q_SEM, max, 0, n); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return q_new(Q_MUTEX, 1, 0, 1); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return q_new(Q_RMUTEX, 1, 0, 1); }

void vQueueDelete(QueueHandle_t q) {
    if (!q) return;
    os_mutex_free(q->lock);
    os_cond_free(q->cond);
    free(q->buf);
    free(q);
}

BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item, TickType_t wait, bool front) {
    uint64_t end = deadline_of(wait);
    os_mutex_lock(q->lock);
    while (q->count == q->len) {
        uint32_t ms = left(end);
        if (!ms) { os_mutex_unlock(q->lock); return errQUEUE_FULL; }
        os_cond_wait(q->cond, q->lock, ms);
    }
    UBaseType_t slot;
    if (front) { q->head = (q->head + q->len - 1) % q->len; slot = q->head; }
    else slot = (q->head + q->count) % q->len;
    memcpy(q->buf + (size_t)slot * q->size, item, q->size);
    q->count++;
    os_cond_broadcast(q->cond);
    os_mutex_unlock(q->lock);
    return pdPASS;
}

static BaseType_t q_take(QueueHandle_t q, void *out, TickType_t wait, bool remove) {
    uint64_t end = deadline_of(wait);
    os_mutex_lock(q->lock);
    while (!q->count) {
        uint32_t ms = left(end);
        if (!ms) { os_mutex_unlock(q->lock); return pdFAIL; }
        os_cond_wait(q->cond, q->lock, ms);
    }
    if (out) memcpy(out, q->buf + (size_t)q->head * q->size, q->size);
    if (remove) {
        q->head = (q->head + 1) % q->len;
        q->count--;
        os_cond_broadcast(q->cond);
    }
    os_mutex_unlock(q->lock);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait) { return q_take(q, out, wait, true); }
BaseType_t xQueuePeek(QueueHandle_t q, void *out, TickType_t wait) { return q_take(q, out, wait, false); }

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    os_mutex_lock(q->lock);
    UBaseType_t n = q->count;
    os_mutex_unlock(q->lock);
    return n;
}

BaseType_t xQueueReset(QueueHandle_t q) {
    os_mutex_lock(q->lock);
    q->head = q->count = 0;
    os_cond_broadcast(q->cond);
    os_mutex_unlock(q->lock);
    return pdPASS;
}

/* Owner is the calling thread's task when it has one; threads microlink did not create
 * (the caller of microlink_start, lwIP's tcpip thread) count by address of a TLS slot. */
static _Thread_local char me_slot;
static uintptr_t me(void) { return (uintptr_t)&me_slot; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (s->kind == Q_QUEUE) return q_take(s, NULL, wait, true);
    uint64_t end = deadline_of(wait);
    os_mutex_lock(s->lock);
    if (s->kind == Q_RMUTEX && s->depth && s->owner_id == me()) {
        s->depth++;
        os_mutex_unlock(s->lock);
        return pdPASS;
    }
    while (!s->count) {
        uint32_t ms = left(end);
        if (!ms) { os_mutex_unlock(s->lock); return pdFAIL; }
        os_cond_wait(s->cond, s->lock, ms);
    }
    s->count--;
    if (s->kind != Q_SEM) { s->owner_id = me(); s->depth = 1; }
    os_mutex_unlock(s->lock);
    return pdPASS;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    os_mutex_lock(s->lock);
    if (s->kind == Q_RMUTEX || s->kind == Q_MUTEX) {
        if (!s->depth || s->owner_id != me()) { os_mutex_unlock(s->lock); return pdFAIL; }
        if (--s->depth) { os_mutex_unlock(s->lock); return pdPASS; }
        s->owner_id = 0;
    } else if (s->count == s->len) {
        os_mutex_unlock(s->lock);
        return pdFAIL;
    }
    s->count++;
    os_cond_broadcast(s->cond);
    os_mutex_unlock(s->lock);
    return pdPASS;
}

/* ---- event groups ---- */

struct ts_events {
    os_mutex_t *lock;
    os_cond_t *cond;
    EventBits_t bits;
};

EventGroupHandle_t xEventGroupCreate(void) {
    struct ts_events *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->lock = os_mutex_new(false);
    e->cond = os_cond_new();
    if (!e->lock || !e->cond) { vEventGroupDelete(e); return NULL; }
    return e;
}

void vEventGroupDelete(EventGroupHandle_t e) {
    if (!e) return;
    os_mutex_free(e->lock);
    os_cond_free(e->cond);
    free(e);
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits) {
    os_mutex_lock(e->lock);
    e->bits |= bits;
    EventBits_t v = e->bits;
    os_cond_broadcast(e->cond);
    os_mutex_unlock(e->lock);
    return v;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits) {
    os_mutex_lock(e->lock);
    EventBits_t v = e->bits;   /* FreeRTOS returns the value before clearing */
    e->bits &= ~bits;
    os_mutex_unlock(e->lock);
    return v;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t e) {
    os_mutex_lock(e->lock);
    EventBits_t v = e->bits;
    os_mutex_unlock(e->lock);
    return v;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait) {
    uint64_t end = deadline_of(wait);
    os_mutex_lock(e->lock);
    for (;;) {
        EventBits_t hit = e->bits & bits;
        if (all ? hit == bits : hit != 0) {
            EventBits_t v = e->bits;
            if (clear) e->bits &= ~bits;
            os_mutex_unlock(e->lock);
            return v;
        }
        uint32_t ms = left(end);
        if (!ms) break;
        os_cond_wait(e->cond, e->lock, ms);
    }
    EventBits_t v = e->bits;   /* timeout: current bits, nothing cleared */
    os_mutex_unlock(e->lock);
    return v;
}

/* ---- critical sections ---- */

static os_mutex_t *critical;

void ts_critical_enter(void) {
    if (!critical) critical = os_mutex_new(true);   /* first use is from one thread at start */
    os_mutex_lock(critical);
}

void ts_critical_exit(void) { os_mutex_unlock(critical); }
