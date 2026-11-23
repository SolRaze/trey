/* Force-included before every source: names newer toolchains have and ours lack. */
#ifndef TS_COMPAT_H
#define TS_COMPAT_H

/* mbedTLS 3 names on 2.28 (the console's portlib) */
#define MBEDTLS_SSL_VERSION_TLS1_2 0x0303
#define MBEDTLS_SSL_VERSION_TLS1_3 0x0304
#define mbedtls_ssl_conf_max_tls_version(conf, v) \
    mbedtls_ssl_conf_max_version((conf), 3, (v) == MBEDTLS_SSL_VERSION_TLS1_2 ? 3 : 4)
#define mbedtls_ssl_conf_min_tls_version(conf, v) \
    mbedtls_ssl_conf_min_version((conf), 3, (v) == MBEDTLS_SSL_VERSION_TLS1_2 ? 3 : 4)

#endif
