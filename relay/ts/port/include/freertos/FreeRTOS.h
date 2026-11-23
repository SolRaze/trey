/* FreeRTOS API subset microlink and wireguard-lwip call, over port/os.h (rtos.c).
 * Ticks are milliseconds: configTICK_RATE_HZ 1000. task.h, queue.h, semphr.h and
 * event_groups.h all land here. */
#ifndef TS_FREERTOS_H
#define TS_FREERTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef void (*TaskFunction_t)(void *);

/* esp_bit_defs.h */
#define BIT0 (1u << 0)
#define BIT1 (1u << 1)
#define BIT2 (1u << 2)
#define BIT3 (1u << 3)
#define BIT4 (1u << 4)
#define BIT5 (1u << 5)
#define BIT6 (1u << 6)
#define BIT7 (1u << 7)
#define BIT8 (1u << 8)
#define BIT9 (1u << 9)
#define BIT10 (1u << 10)
#define BIT11 (1u << 11)
#define BIT12 (1u << 12)
#define BIT13 (1u << 13)
#define BIT14 (1u << 14)
#define BIT15 (1u << 15)
#define BIT16 (1u << 16)
#define BIT17 (1u << 17)
#define BIT18 (1u << 18)
#define BIT19 (1u << 19)
#define BIT20 (1u << 20)
#define BIT21 (1u << 21)
#define BIT22 (1u << 22)
#define BIT23 (1u << 23)

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define errQUEUE_FULL 0
#define portMAX_DELAY ((TickType_t)UINT32_MAX)
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define portTICK_RATE_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))
#define tskNO_AFFINITY (-1)
#define portNUM_PROCESSORS 1
#define configMAX_PRIORITIES 25
#define tskIDLE_PRIORITY 0
#define portYIELD_FROM_ISR(x) ((void)(x))
#define configASSERT(x) ((void)0)

typedef struct ts_task *TaskHandle_t;
typedef struct ts_queue *QueueHandle_t;
typedef struct ts_queue *SemaphoreHandle_t;
typedef struct ts_events *EventGroupHandle_t;
/* Storage for the *Static creators; the shim allocates anyway and ignores it. */
typedef struct { uint8_t unused; } StaticSemaphore_t;

/* tasks */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) \
    xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
void vTaskDelete(TaskHandle_t t);           /* NULL only: ends the calling task */
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
static inline BaseType_t xPortGetCoreID(void) { return 0; }

/* queues */
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item, TickType_t wait, bool front);
#define xQueueSend(q, item, wait) xQueueGenericSend(q, item, wait, false)
#define xQueueSendToBack(q, item, wait) xQueueGenericSend(q, item, wait, false)
#define xQueueSendToFront(q, item, wait) xQueueGenericSend(q, item, wait, true)
#define xQueueSendFromISR(q, item, woken) xQueueGenericSend(q, item, 0, false)
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait);
BaseType_t xQueuePeek(QueueHandle_t q, void *out, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
BaseType_t xQueueReset(QueueHandle_t q);

/* semaphores and mutexes: a queue of zero-size items, as in FreeRTOS */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
#define xSemaphoreCreateBinaryStatic(buf) xSemaphoreCreateBinary()
#define xSemaphoreCreateMutexStatic(buf) xSemaphoreCreateMutex()
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
#define xSemaphoreTakeRecursive(s, wait) xSemaphoreTake(s, wait)
#define xSemaphoreGiveRecursive(s) xSemaphoreGive(s)
#define xSemaphoreGiveFromISR(s, woken) xSemaphoreGive(s)
#define vSemaphoreDelete(s) vQueueDelete(s)

/* event groups */
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t e);
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t e);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, BaseType_t clear,
                                BaseType_t all, TickType_t wait);

/* critical sections: one global recursive lock */
void ts_critical_enter(void);
void ts_critical_exit(void);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMUX_INITIALIZE(mux) (*(mux) = 0)
#define taskENTER_CRITICAL(mux) ts_critical_enter()
#define taskEXIT_CRITICAL(mux) ts_critical_exit()
#define portENTER_CRITICAL(mux) ts_critical_enter()
#define portEXIT_CRITICAL(mux) ts_critical_exit()

#endif
