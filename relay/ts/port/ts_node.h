/* Relay's API to the Tailscale node, port/node.c. Plain C, no engine headers. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int ts_node_start(const char *dir, const char *key_path, const char *name);
bool ts_node_line(char *out, size_t cap);   /* true once on the tailnet; out = "100.x n peers" */
void ts_node_stop(void);

#ifdef __cplusplus
}
#endif
