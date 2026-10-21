/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys_clock.h>

#include <papermono/settings.h>
#include <papermono/time_sync.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_time_sync, LOG_LEVEL_INF);

#define TIME_SYNC_STACK_SIZE 3072
#define TIME_SYNC_PRIORITY 9
#define TIME_SYNC_QUERY_TIMEOUT_MS 4000U
#define TIME_SYNC_MIN_EPOCH 1577836800ULL
#define TIME_SYNC_MAX_EPOCH 4102444800ULL

static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
static bool initialized;
K_MUTEX_DEFINE(sync_lock);
K_SEM_DEFINE(sync_request, 0, 1);

static int set_clocks(const struct sntp_time *sample)
{
	const struct papermono_preferences *preferences = papermono_settings_get();
	struct timespec utc = {
		.tv_sec = (time_t)sample->seconds,
		.tv_nsec = (long)(((uint64_t)sample->fraction * NSEC_PER_SEC) >> 32),
	};
	time_t local_epoch = utc.tv_sec +
		(time_t)preferences->timezone_offset_minutes * 60;
	struct tm local_tm;
	struct rtc_time local_rtc = { 0 };
	int ret;

	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &utc);
	if (ret < 0) {
		return ret;
	}
	if (!device_is_ready(rtc)) {
		return -ENODEV;
	}
	if (gmtime_r(&local_epoch, &local_tm) == NULL) {
		return -ERANGE;
	}
	*rtc_time_to_tm(&local_rtc) = local_tm;
	local_rtc.tm_nsec = utc.tv_nsec;
	return rtc_set_time(rtc, &local_rtc);
}

static void time_sync_thread(void *first, void *second, void *third)
{
	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);

	for (;;) {
		k_sem_take(&sync_request, K_FOREVER);
		k_mutex_lock(&sync_lock, K_FOREVER);
		if (!initialized) {
			k_mutex_unlock(&sync_lock);
			continue;
		}
		k_mutex_unlock(&sync_lock);

		int ret = -EIO;
		struct sntp_time sample = { 0 };
		for (unsigned int attempt = 0U;
		     attempt < CONFIG_PAPERMONO_NTP_RETRY_COUNT; ++attempt) {
			if (attempt > 0U) {
				k_sleep(K_SECONDS(5U * attempt));
			}
			ret = sntp_simple(CONFIG_PAPERMONO_NTP_SERVER,
				TIME_SYNC_QUERY_TIMEOUT_MS, &sample);
			if (ret == 0 && (sample.seconds < TIME_SYNC_MIN_EPOCH ||
			    sample.seconds >= TIME_SYNC_MAX_EPOCH)) {
				ret = -ERANGE;
			}
			if (ret == 0) {
				ret = set_clocks(&sample);
			}
			if (ret == 0) {
				break;
			}
			LOG_WRN("SNTP attempt %u failed (%d)", attempt + 1U, ret);
		}

		if (ret == 0) {
			LOG_INF("RX8130 calibrated from SNTP epoch %llu",
				(unsigned long long)sample.seconds);
		}
		papermono_ui_request_background_refresh();
	}
}

K_THREAD_DEFINE(time_sync_tid, TIME_SYNC_STACK_SIZE, time_sync_thread,
	NULL, NULL, NULL, TIME_SYNC_PRIORITY, 0, 0);

int papermono_time_sync_init(void)
{
	k_mutex_lock(&sync_lock, K_FOREVER);
	if (!initialized) {
		initialized = true;
	}
	k_mutex_unlock(&sync_lock);
	return device_is_ready(rtc) ? 0 : -ENODEV;
}

void papermono_time_sync_request(void)
{
	bool ready;

	k_mutex_lock(&sync_lock, K_FOREVER);
	ready = initialized;
	k_mutex_unlock(&sync_lock);
	if (ready) {
		k_sem_give(&sync_request);
	}
}
