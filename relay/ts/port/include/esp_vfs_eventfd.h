/* No eventfd here: registration fails, so the DERP task falls back to polling. */
#ifndef TS_ESP_VFS_EVENTFD_H
#define TS_ESP_VFS_EVENTFD_H
#include <errno.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { size_t max_fds; } esp_vfs_eventfd_config_t;
#define ESP_VFS_EVENTD_CONFIG_DEFAULT() { .max_fds = 1 }
static inline esp_err_t esp_vfs_eventfd_register(const esp_vfs_eventfd_config_t *c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
static inline int eventfd(unsigned int initval, int flags) { (void)initval; (void)flags; errno = ENOSYS; return -1; }
#endif
