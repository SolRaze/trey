/* ESP-IDF runtime pieces microlink calls: log, timer, random, crc32, error names, MAC. */
#include "esp_crc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "os.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static esp_log_level_t level = TS_LOG_LEVEL;

void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; level = l; }
esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return level; }

void ts_log(esp_log_level_t l, const char *tag, const char *fmt, ...) {
    if (l > level) return;
    static const char mark[] = "NEWIDV";
    char line[256];
    int n = snprintf(line, sizeof line, "%c %s: ", mark[l], tag);
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && n < (int)sizeof line) vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    os_log_line(line);
}

int64_t esp_timer_get_time(void) { return (int64_t)os_us(); }

uint32_t esp_random(void) {
    uint32_t v;
    os_random(&v, sizeof v);
    return v;
}

void esp_fill_random(void *buf, size_t len) { os_random(buf, len); }
unsigned int ts_lwip_rand(void) { return esp_random(); }

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

const char *esp_err_to_name(esp_err_t e) {
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR";
    }
}

/* Device id: random once, kept in the NVS store so the name suffix stays put. */
#include "nvs.h"
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    (void)type;
    nvs_handle_t h;
    size_t len = 6;
    if (nvs_open("ts_port", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t r = nvs_get_blob(h, "mac", mac, &len);
        if (r != ESP_OK || len != 6) {
            os_random(mac, 6);
            mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);   /* unicast, locally administered */
            nvs_set_blob(h, "mac", mac, 6);
            nvs_commit(h);
        }
        nvs_close(h);
        return ESP_OK;
    }
    os_random(mac, 6);
    mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);
    return ESP_OK;
}

/* Firmware OTA is the Relay updater's job: microlink's rollback hooks do nothing here. */
void ml_ota_check_pending(void) {}
void ml_ota_confirm(void) {}
