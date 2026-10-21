/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/drivers/rtc.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_RTC_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_RTC_H_

#include <zephyr/device.h>

struct rtc_time {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

int rtc_get_time(const struct device *dev, struct rtc_time *timeptr);
int rtc_set_time(const struct device *dev, const struct rtc_time *timeptr);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_RTC_H_ */
