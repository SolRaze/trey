/* Mac test node: joins the tailnet with the same engine the console runs, then echoes UDP 9000.
 * usage: tsnode <keyfile> <store-dir> [name]
 *   keyfile   auth key file, `key tskey-...` or the bare key on its first line
 *   store-dir node keys and peer cache, kept between runs
 * Check from another tailnet machine: ping <its 100.x> | echo hi | nc -u -w1 <its 100.x> 9000 */
#include "microlink.h"
#include "nvs.h"
#include "os.h"

#include "lwip/tcpip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int ts_ca_set(const unsigned char *pem, size_t len);

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = '\0'; if (len) *len = (size_t)n + 1; }
    return b;
}

/* The key line: "key <value>" or the bare value. Never printed. */
static char *read_key(const char *path) {
    char *b = slurp(path, NULL);
    if (!b) return NULL;
    char *k = strncmp(b, "key ", 4) == 0 ? b + 4 : b;
    k[strcspn(k, "\r\n \t")] = '\0';
    char *out = strdup(k);
    memset(b, 0, strlen(b));
    free(b);
    return out;
}

static void on_udp(microlink_udp_socket_t *s, uint32_t ip, uint16_t port,
                   const uint8_t *data, size_t len, void *u) {
    char a[16];
    microlink_ip_to_str(ip, a);
    printf("udp %zu bytes from %s:%u, echoing\n", len, a, port);
    microlink_udp_send(s, ip, port, data, len);
}

static void on_state(microlink_t *ml, microlink_state_t st, void *u) {
    printf("state %d\n", (int)st);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: tsnode <keyfile> <store-dir> [name]\n"); return 2; }
    char *key = read_key(argv[1]);
    if (!key || !*key) { fprintf(stderr, "no key in %s\n", argv[1]); return 1; }
    mkdir(argv[2], 0700);
    ts_nvs_set_dir(argv[2]);

    /* the console's built-in roots, so a Mac run proves them */
    if (ts_ca_set(NULL, 0) != 0) { fprintf(stderr, "CA load failed\n"); return 1; }

    tcpip_init(NULL, NULL);

    microlink_config_t cfg = {
        .auth_key = key,
        .device_name = argc > 3 ? argv[3] : "trey-mac-test",
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 16,
    };
    microlink_t *ml = microlink_init(&cfg);
    if (!ml) { fprintf(stderr, "microlink_init failed\n"); return 1; }
    microlink_set_state_callback(ml, on_state, NULL);
    if (microlink_start(ml) != ESP_OK) { fprintf(stderr, "microlink_start failed\n"); return 1; }

    microlink_udp_socket_t *u = NULL;
    for (uint64_t t0 = os_ms(); ; os_sleep_ms(1000)) {
        if (!u && microlink_is_connected(ml)) {
            char a[16];
            microlink_ip_to_str(microlink_get_vpn_ip(ml), a);
            printf("up as %s, %d peers\n", a, microlink_get_peer_count(ml));
            fflush(stdout);
            u = microlink_udp_create(ml, 9000);
            if (u) microlink_udp_set_rx_callback(u, on_udp, NULL);
        }
        if (!u && os_ms() - t0 > 120000) { fprintf(stderr, "not up after 120 s\n"); return 1; }
    }
}
