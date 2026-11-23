/* CA roots for microlink's TLS (DERP, and control when CONFIG_ML_CTRL_TLS). The embedder hands
 * over a PEM bundle once with ts_ca_set, or NULL for the built-in roots in ca_roots.h; every
 * config gets the same parsed chain. */
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "ca_roots.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

static mbedtls_x509_crt chain;
static int parsed;   /* 0 none, 1 ok, -1 failed */

/* pem must be NUL-terminated, len counting the NUL, as mbedtls_x509_crt_parse wants. */
int ts_ca_set(const unsigned char *pem, size_t len) {
    if (!pem) { pem = (const unsigned char *)ts_ca_builtin; len = sizeof ts_ca_builtin; }
    mbedtls_x509_crt_init(&chain);
    int r = mbedtls_x509_crt_parse(&chain, pem, len);
    parsed = r < 0 ? -1 : 1;   /* r > 0 = that many roots skipped, the rest loaded */
    if (r > 0) ESP_LOGW("ts_ca", "%d roots unreadable, skipped", r);
    return r < 0 ? r : 0;
}

esp_err_t esp_crt_bundle_attach(void *conf) {
    if (parsed != 1) {
        ESP_LOGE("ts_ca", "no CA roots loaded");
        return ESP_FAIL;
    }
    mbedtls_ssl_conf_ca_chain((mbedtls_ssl_config *)conf, &chain, NULL);
    return ESP_OK;
}
