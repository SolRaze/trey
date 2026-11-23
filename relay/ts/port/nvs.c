/* NVS on files. <dir>/<namespace>.<key>, one value per file, written to a temp name and
 * renamed so a power cut leaves the old value or the new one. Handles index a small table. */
#include "nvs.h"
#include "os.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPEN 8
#define NS_MAX 16

static char base[128] = "ts";
static char open_ns[MAX_OPEN][NS_MAX];
static os_mutex_t *lock;

void ts_nvs_set_dir(const char *dir) { snprintf(base, sizeof base, "%s", dir); }

static void lock_init(void) { if (!lock) lock = os_mutex_new(false); }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    if (!ns || strlen(ns) >= NS_MAX) return ESP_ERR_INVALID_ARG;
    lock_init();
    os_mutex_lock(lock);
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!open_ns[i][0]) {
            strcpy(open_ns[i], ns);
            os_mutex_unlock(lock);
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    os_mutex_unlock(lock);
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t h) {
    if (h < 1 || h > MAX_OPEN) return;
    os_mutex_lock(lock);
    open_ns[h - 1][0] = '\0';
    os_mutex_unlock(lock);
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }   /* every set is durable */

static bool path_of(nvs_handle_t h, const char *key, char *out, size_t cap) {
    if (h < 1 || h > MAX_OPEN || !open_ns[h - 1][0] || !key) return false;
    int n = snprintf(out, cap, "%s/%s.%s", base, open_ns[h - 1], key);
    return n > 0 && (size_t)n < cap;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    char p[256];
    if (!path_of(h, key, p, sizeof p)) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NVS_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return ESP_FAIL; }
    if (!out) { fclose(f); *len = (size_t)size; return ESP_OK; }   /* length query */
    if ((size_t)size > *len) { fclose(f); *len = (size_t)size; return ESP_ERR_NVS_INVALID_LENGTH; }
    size_t got = fread(out, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return ESP_FAIL;
    *len = got;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len) {
    char p[256], tmp[262];
    if (!path_of(h, key, p, sizeof p)) return ESP_ERR_INVALID_ARG;
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool ok = fwrite(val, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return ESP_FAIL; }
    remove(p);   /* FAT rename does not replace */
    return rename(tmp, p) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    char p[256];
    if (!path_of(h, key, p, sizeof p)) return ESP_ERR_INVALID_ARG;
    return remove(p) == 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

/* Every file of the namespace: names starting "<ns>." in the store directory. */
esp_err_t nvs_erase_all(nvs_handle_t h) {
    if (h < 1 || h > MAX_OPEN || !open_ns[h - 1][0]) return ESP_ERR_INVALID_ARG;
    char prefix[NS_MAX + 1], p[256];
    snprintf(prefix, sizeof prefix, "%s.", open_ns[h - 1]);
    DIR *d = opendir(base);
    if (!d) return ESP_OK;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
        if (snprintf(p, sizeof p, "%s/%s", base, e->d_name) < (int)sizeof p) remove(p);
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    size_t cap = out ? *len : 0;
    size_t n = cap ? cap - 1 : 0;
    esp_err_t r = nvs_get_blob(h, key, out, out ? &n : len);
    if (r == ESP_ERR_NVS_INVALID_LENGTH) { *len = n + 1; return r; }
    if (r != ESP_OK) return r;
    if (!out) { (*len)++; return ESP_OK; }
    out[n] = '\0';
    *len = n + 1;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) {
    return nvs_set_blob(h, key, val, strlen(val));
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    size_t n = 1;
    return nvs_get_blob(h, key, out, &n);
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) { return nvs_set_blob(h, key, &val, 1); }

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    size_t n = 4;
    return nvs_get_blob(h, key, out, &n);
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t val) { return nvs_set_blob(h, key, &val, 4); }

esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out) {
    size_t n = 8;
    return nvs_get_blob(h, key, out, &n);
}

esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t val) { return nvs_set_blob(h, key, &val, 8); }
