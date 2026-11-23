#ifndef TS_ESP_CRC_H
#define TS_ESP_CRC_H
#include <stddef.h>
#include <stdint.h>
/* CRC-32 (IEEE, reflected) continuing from crc, as ESP-IDF's esp_crc32_le. */
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
#endif
