/* A stable per-device 6-byte id stands in for the Wi-Fi MAC (device name suffix). */
#ifndef TS_ESP_MAC_H
#define TS_ESP_MAC_H
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
#endif
