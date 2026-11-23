/* lwIP compiler and platform glue. */
#ifndef TS_ARCH_CC_H
#define TS_ARCH_CC_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwip assert: %s %s:%d\n", x, __FILE__, __LINE__); abort(); } while (0)
#define LWIP_RAND() ((u32_t)ts_lwip_rand())
unsigned int ts_lwip_rand(void);
#define LWIP_NO_CTYPE_H 0
#endif
