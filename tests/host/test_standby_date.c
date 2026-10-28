/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <papermono/standby_date.h>

static void expect_date(int year, int month, int day,
	int next_year, int next_month, int next_day)
{
	struct papermono_calendar_time tomorrow = {
		.year = 2001, .month = 8, .day = 7,
		.hour = 17, .minute = 33, .second = 59,
		.all_day = true, .utc = true,
	};

	assert(papermono_standby_tomorrow(year, month, day, &tomorrow) == 0);
	assert(tomorrow.year == next_year);
	assert(tomorrow.month == next_month);
	assert(tomorrow.day == next_day);
	assert(tomorrow.hour == 0 && tomorrow.minute == 0 && tomorrow.second == 0);
	assert(!tomorrow.all_day && !tomorrow.utc);
}

static void expect_error(int year, int month, int day, int error)
{
	struct papermono_calendar_time tomorrow = {
		.year = 2001, .month = 8, .day = 7,
		.hour = 17, .minute = 33, .second = 59,
		.all_day = true, .utc = true,
	};

	assert(papermono_standby_tomorrow(year, month, day, &tomorrow) == error);
	assert(tomorrow.year == 2001 && tomorrow.month == 8 && tomorrow.day == 7);
	assert(tomorrow.hour == 17 && tomorrow.minute == 33 && tomorrow.second == 59);
	assert(tomorrow.all_day && tomorrow.utc);
}

int main(void)
{
	expect_date(2026, 10, 26, 2026, 10, 27);
	expect_date(2026, 1, 31, 2026, 2, 1);
	expect_date(2026, 4, 30, 2026, 5, 1);
	expect_date(2026, 12, 31, 2027, 1, 1);
	expect_date(2024, 2, 28, 2024, 2, 29);
	expect_date(2024, 2, 29, 2024, 3, 1);
	expect_date(2023, 2, 28, 2023, 3, 1);
	expect_date(2000, 2, 28, 2000, 2, 29);
	expect_date(1900, 2, 28, 1900, 3, 1);
	expect_date(2100, 2, 28, 2100, 3, 1);
	expect_date(1, 1, 1, 1, 1, 2);
	expect_date(INT16_MAX, 12, 30, INT16_MAX, 12, 31);

	expect_error(2023, 2, 29, -EINVAL);
	expect_error(2024, 2, 30, -EINVAL);
	expect_error(2026, 4, 31, -EINVAL);
	expect_error(2026, 1, 0, -EINVAL);
	expect_error(2026, 1, -1, -EINVAL);
	expect_error(2026, 1, 32, -EINVAL);
	expect_error(2026, 0, 1, -EINVAL);
	expect_error(2026, -1, 1, -EINVAL);
	expect_error(2026, 13, 1, -EINVAL);
	expect_error(0, 1, 1, -EINVAL);
	expect_error(-1, 1, 1, -EINVAL);
	expect_error(INT32_MAX, 1, 1, -EINVAL);
	expect_error(INT16_MAX, 12, 31, -ERANGE);
	assert(papermono_standby_tomorrow(2026, 10, 26, NULL) == -EINVAL);
	puts("standby Gregorian tomorrow: PASS");
	return 0;
}
