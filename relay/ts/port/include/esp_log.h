/* ESP_LOGx onto port/os.h log lines. Level per build: TS_LOG_LEVEL, default ESP_LOG_INFO. */
#ifndef TS_ESP_LOG_H
#define TS_ESP_LOG_H

#include "esp_err.h"

typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE
} esp_log_level_t;

#ifndef TS_LOG_LEVEL
#define TS_LOG_LEVEL ESP_LOG_INFO
#endif
#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL TS_LOG_LEVEL
#endif

void ts_log(esp_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);

#define TS_LOG_AT(l, tag, ...) do { if ((l) <= LOG_LOCAL_LEVEL) ts_log(l, tag, __VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) TS_LOG_AT(ESP_LOG_ERROR, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) TS_LOG_AT(ESP_LOG_WARN, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) TS_LOG_AT(ESP_LOG_INFO, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) TS_LOG_AT(ESP_LOG_DEBUG, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) TS_LOG_AT(ESP_LOG_VERBOSE, tag, __VA_ARGS__)

#endif
