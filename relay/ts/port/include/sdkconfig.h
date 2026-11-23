/* Build options ESP-IDF would generate from microlink's Kconfig: its defaults, except
 * where the console differs (commented). */
#ifndef TS_SDKCONFIG_H
#define TS_SDKCONFIG_H

#define CONFIG_ML_MAX_PEERS 16
#define CONFIG_ML_DERP_VERIFY_CERT 1
#define CONFIG_ML_DERP_REGION 9
#define CONFIG_ML_PRIORITY_PEER_IP ""
#define CONFIG_ML_NVS_MAX_PEERS 64
/* map-response buffers: Relay's heap is large, the core's is not; revisit for the core */
#define CONFIG_ML_H2_BUFFER_SIZE_KB 512
#define CONFIG_ML_JSON_BUFFER_SIZE_KB 512
#define CONFIG_ML_FILTER_MAX_ENTRIES 4096
#define CONFIG_ML_ADVERTISE_ROUTES ""
#define CONFIG_ML_UDP_RX_TASK_PRIO 7
#define CONFIG_ML_STOP_TIMEOUT_MS 30000
#define CONFIG_ML_LOG_LEVEL 2
#define CONFIG_ML_DEVICE_NAME "trey"
#define CONFIG_ML_CTRL_HOST "controlplane.tailscale.com"
#define CONFIG_ML_CTRL_NOISE_PUBKEY_HEX ""
#define CONFIG_ML_CTRL_TLS_VERIFY_CERT 1
#define CONFIG_ML_CONFIG_MAX_ALLOWED_PEERS 16
#define CONFIG_ML_TAILSCALE_AUTH_KEY ""
#define CONFIG_ML_NET_SWITCH_FAILBACK_MS 120000
/* off: no subnet router, no IPv6 router path, no zero-copy (it sends on an lwIP underlay
 * the console does not have), no OTA, no cellular, no web config */
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1
#define CONFIG_MBEDTLS_ERROR_STRINGS 1
#define CONFIG_FREERTOS_UNICORE 1
#define CONFIG_LOG_MAXIMUM_LEVEL 3

#endif
