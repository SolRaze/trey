/* One heap: every capability is plain malloc. */
#ifndef TS_ESP_HEAP_CAPS_H
#define TS_ESP_HEAP_CAPS_H
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DEFAULT  (1u << 12)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM   (1u << 10)
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 0; }
static inline size_t esp_get_free_heap_size(void) { return 0; }
#endif
