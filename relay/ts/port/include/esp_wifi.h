/* Radio power is the console's own business. */
#ifndef TS_ESP_WIFI_H
#define TS_ESP_WIFI_H
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t esp_wifi_set_max_tx_power(int8_t p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
#endif
