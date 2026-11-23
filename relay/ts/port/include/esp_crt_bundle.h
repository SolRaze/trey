/* CA roots for DERP and control TLS: the PEM file the build embeds (ts_ca_pem). */
#ifndef TS_ESP_CRT_BUNDLE_H
#define TS_ESP_CRT_BUNDLE_H
#include "esp_err.h"
esp_err_t esp_crt_bundle_attach(void *ssl_conf);
#endif
