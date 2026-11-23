/* Microseconds since start, as esp_timer_get_time. */
#ifndef TS_ESP_TIMER_H
#define TS_ESP_TIMER_H
#include <stdint.h>
int64_t esp_timer_get_time(void);
#endif
