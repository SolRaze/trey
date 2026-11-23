/* No ESP netifs: microlink's LAN lookups find none, so it advertises no LAN endpoint. */
#ifndef TS_ESP_NETIF_H
#define TS_ESP_NETIF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } esp_ip6_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef bool (*esp_netif_find_predicate_t)(esp_netif_t *netif, void *ctx);
#define ESP_NETIF_FLAG_IS_PPP (1u << 6)

static inline esp_netif_t *esp_netif_get_default_netif(void) { return NULL; }
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { (void)k; return NULL; }
static inline esp_netif_t *esp_netif_find_if(esp_netif_find_predicate_t f, void *c) { (void)f; (void)c; return NULL; }
static inline unsigned esp_netif_get_flags(esp_netif_t *n) { (void)n; return 0; }
static inline const char *esp_netif_get_ifkey(esp_netif_t *n) { (void)n; return ""; }
static inline bool esp_netif_is_netif_up(esp_netif_t *n) { (void)n; return false; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; (void)i; return ESP_FAIL; }
static inline esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *n, esp_ip6_addr_t *a) { (void)n; (void)a; return ESP_FAIL; }
static inline esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *n) { (void)n; return ESP_FAIL; }
#endif
