/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/sys/util.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_SYS_UTIL_H_
#define PAPERMONO_PREVIEW_ZEPHYR_SYS_UTIL_H_

#include <stddef.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define BIT(bit) (1UL << (bit))
#define BIT64(bit) (1ULL << (bit))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (31U - (h))))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define ABS(x) (((x) < 0) ? -(x) : (x))
#define ROUND_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define ROUND_DOWN(x, align) ((x) & ~((align) - 1))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + ((d) / 2)) / (d))
#define CONTAINER_OF(ptr, type, field) \
	((type *)(((char *)(ptr)) - offsetof(type, field)))
#define ARG_UNUSED(x) (void)(x)
#define __maybe_unused __attribute__((unused))
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define IS_ENABLED(cfg) 0
#define KB(x) ((x) * 1024U)
#define MB(x) (KB(x) * 1024U)

#endif /* PAPERMONO_PREVIEW_ZEPHYR_SYS_UTIL_H_ */
