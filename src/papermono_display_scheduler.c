/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <papermono/display_scheduler.h>
#include <papermono/ssd1677.h>

LOG_MODULE_REGISTER(papermono_display_scheduler, LOG_LEVEL_INF);

static const struct device *scheduled_display;
static papermono_display_input_guard_t scheduled_guard;
static void *scheduled_guard_context;
K_MUTEX_DEFINE(refresh_lock);

int papermono_display_scheduler_init(const struct device *display,
	papermono_display_input_guard_t input_guard, void *context)
{
	if (display == NULL || !device_is_ready(display)) {
		return -ENODEV;
	}
	scheduled_display = display;
	scheduled_guard = input_guard;
	scheduled_guard_context = context;
	return 0;
}

int papermono_display_refresh(enum papermono_refresh_quality quality)
{
	int ret;

	if (scheduled_display == NULL) {
		return -ENODEV;
	}
	if (quality != PAPERMONO_REFRESH_AUTO &&
	    quality != PAPERMONO_REFRESH_FAST &&
	    quality != PAPERMONO_REFRESH_FULL) {
		return -EINVAL;
	}
	k_mutex_lock(&refresh_lock, K_FOREVER);
	if (scheduled_guard != NULL) {
		scheduled_guard(true, scheduled_guard_context);
	}

	if (quality == PAPERMONO_REFRESH_FULL) {
		ret = papermono_ssd1677_force_full_next(scheduled_display);
		if (ret < 0) {
			LOG_WRN("Unable to request a full refresh (%d)", ret);
			goto release;
		}
	}
	ret = display_blanking_on(scheduled_display);
	if (ret == 0) {
		lv_refr_now(NULL);
		ret = display_blanking_off(scheduled_display);
	}

release:
	/* Keep display switching noise out of the touch event stream. */
	k_msleep(CONFIG_PAPERMONO_DISPLAY_INPUT_SETTLE_MS);
	if (scheduled_guard != NULL) {
		scheduled_guard(false, scheduled_guard_context);
	}
	k_mutex_unlock(&refresh_lock);
	return ret;
}
