/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_DISPLAY_SCHEDULER_H_
#define PAPERMONO_DISPLAY_SCHEDULER_H_

#include <stdbool.h>

#include <zephyr/device.h>

#include <papermono/refresh.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*papermono_display_input_guard_t)(bool blocked, void *context);

int papermono_display_scheduler_init(const struct device *display,
	papermono_display_input_guard_t input_guard, void *context);
int papermono_display_refresh(enum papermono_refresh_quality quality);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_DISPLAY_SCHEDULER_H_ */
