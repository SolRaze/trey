/* lwIP OS types over the FreeRTOS shim. */
#ifndef TS_SYS_ARCH_H
#define TS_SYS_ARCH_H
#include "freertos/FreeRTOS.h"

typedef SemaphoreHandle_t sys_sem_t;
typedef SemaphoreHandle_t sys_mutex_t;
typedef QueueHandle_t sys_mbox_t;
typedef TaskHandle_t sys_thread_t;
typedef int sys_prot_t;

#define sys_sem_valid(s) (*(s) != NULL)
#define sys_sem_set_invalid(s) (*(s) = NULL)
#define sys_mutex_valid(m) (*(m) != NULL)
#define sys_mutex_set_invalid(m) (*(m) = NULL)
#define sys_mbox_valid(m) (*(m) != NULL)
#define sys_mbox_set_invalid(m) (*(m) = NULL)

/* ESP-lwIP extension microlink calls: is the calling thread lwIP's tcpip thread?
 * LWIP_CORE_LOCK_QUERY_HOLDER asks "or holds the core lock". */
int sys_thread_tcpip(int query);
#endif
