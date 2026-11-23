/* Cryptographic random from port/os.h. */
#ifndef TS_ESP_RANDOM_H
#define TS_ESP_RANDOM_H
#include <stddef.h>
#include <stdint.h>
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
#endif
