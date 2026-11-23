/* Tailscale node for the Relay app: the embedder's whole view of microlink.
 *   ts_node_start  after socInit; reads the auth key, starts the engine, returns at once
 *   ts_node_line   one status line for the screen and SITE STAT
 *   ts_node_stop   before socExit
 * Files: <dir>/../tskey auth key (`key tskey-...` or bare, never logged) | <dir>/ node keys and
 * peer cache | <dir>/ts.log every engine line, appended. UDP 9000 echoes, the reachability probe. */
#include <3ds.h>

#include "microlink.h"
#include "nvs.h"
#include "os.h"
#include "ts_node.h"

#include "lwip/tcpip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int ts_ca_set(const unsigned char *pem, size_t len);

static LightLock log_lock;
static FILE *log_file;
static microlink_t *ml;
static microlink_udp_socket_t *echo;
static char *key;   /* microlink keeps the pointer and re-reads it on every registration */
static bool tcpip_up, sslc_up;
static char why[48] = "off";

static void log_line(const char *line) {
    LightLock_Lock(&log_lock);
    if (log_file) { fprintf(log_file, "%llu %s\n", (unsigned long long)os_ms(), line); fflush(log_file); }
    LightLock_Unlock(&log_lock);
}

static char *read_key(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char b[256] = { 0 };
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = '\0';
    char *k = strncmp(b, "key ", 4) == 0 ? b + 4 : b;
    k[strcspn(k, "\r\n \t")] = '\0';
    char *out = *k ? strdup(k) : NULL;
    memset(b, 0, sizeof b);
    return out;
}

static void on_udp(microlink_udp_socket_t *s, uint32_t ip, uint16_t port,
                   const uint8_t *data, size_t len, void *u) {
    microlink_udp_send(s, ip, port, data, len);
}

static void on_state(microlink_t *m, microlink_state_t st, void *u) {
    char l[24];
    snprintf(l, sizeof l, "state %d", (int)st);
    log_line(l);
}

int ts_node_start(const char *dir, const char *key_path, const char *name) {
    if (ml) return 0;
    LightLock_Init(&log_lock);
    mkdir(dir, 0777);
    char p[96];
    snprintf(p, sizeof p, "%s/ts.log", dir);
    log_file = fopen(p, "a");
    log_line("node start");

    key = read_key(key_path);
    if (!key) { snprintf(why, sizeof why, "no key"); return -1; }
    if (!sslc_up) {
        if (R_FAILED(sslcInit(0))) { snprintf(why, sizeof why, "sslc failed"); goto fail; }
        sslc_up = true;
    }
    os_init();
    os_set_log(log_line);
    ts_nvs_set_dir(dir);
    if (ts_ca_set(NULL, 0) != 0) { snprintf(why, sizeof why, "CA failed"); goto fail; }
    if (!tcpip_up) { tcpip_init(NULL, NULL); tcpip_up = true; }   /* lwIP has no deinit: once per process */

    microlink_config_t cfg = {
        .auth_key = key,
        .device_name = name,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 16,
    };
    ml = microlink_init(&cfg);
    if (!ml) { snprintf(why, sizeof why, "init failed"); goto fail; }
    microlink_set_state_callback(ml, on_state, NULL);
    if (microlink_start(ml) != ESP_OK) {
        microlink_destroy(ml);
        ml = NULL;
        snprintf(why, sizeof why, "start failed");
        goto fail;
    }
    snprintf(why, sizeof why, "joining");
    return 0;
fail:
    log_line(why);
    memset(key, 0, strlen(key));
    free(key);
    key = NULL;
    return -1;
}

bool ts_node_line(char *out, size_t cap) {
    if (!ml) { snprintf(out, cap, "%s", why); return false; }
    if (!microlink_is_connected(ml)) { snprintf(out, cap, "joining"); return false; }
    char a[16];
    microlink_ip_to_str(microlink_get_vpn_ip(ml), a);
    if (!echo && (echo = microlink_udp_create(ml, 9000))) microlink_udp_set_rx_callback(echo, on_udp, NULL);
    snprintf(out, cap, "%s %d peers", a, microlink_get_peer_count(ml));
    return true;
}

void ts_node_stop(void) {
    if (ml) {
        log_line("node stop");
        microlink_stop(ml);
        microlink_destroy(ml);
        ml = NULL;
        echo = NULL;
    }
    if (key) { memset(key, 0, strlen(key)); free(key); key = NULL; }
    if (sslc_up) { sslcExit(); sslc_up = false; }
    snprintf(why, sizeof why, "off");
    if (log_file) { fclose(log_file); log_file = NULL; }
}
