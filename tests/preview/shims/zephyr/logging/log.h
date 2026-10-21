/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/logging/log.h (all logging is a no-op) */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_LOGGING_LOG_H_
#define PAPERMONO_PREVIEW_ZEPHYR_LOGGING_LOG_H_

#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_WRN 2
#define LOG_LEVEL_INF 3
#define LOG_LEVEL_DBG 4

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)

#define LOG_ERR(...) (void)0
#define LOG_WRN(...) (void)0
#define LOG_INF(...) (void)0
#define LOG_DBG(...) (void)0
#define LOG_HEXDUMP_ERR(...) (void)0
#define LOG_HEXDUMP_INF(...) (void)0
#define LOG_PANIC() (void)0

#endif /* PAPERMONO_PREVIEW_ZEPHYR_LOGGING_LOG_H_ */
