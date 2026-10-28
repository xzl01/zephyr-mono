/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_STANDBY_DATE_H_
#define PAPERMONO_STANDBY_DATE_H_

#include <errno.h>

#include <papermono/personal_data.h>

/**
 * Advance a validated Gregorian date without consulting a clock or timezone.
 * Success returns 0 and writes tomorrow at local midnight (all_day/utc false).
 * Years must fit papermono_calendar_time: 1..INT16_MAX. Invalid input or a null
 * output returns -EINVAL; advancing beyond INT16_MAX returns -ERANGE.
 * The output is unchanged on failure.
 */
static inline int papermono_standby_tomorrow(int year, int month, int day,
	struct papermono_calendar_time *tomorrow)
{
	static const uint8_t month_days[] = {
		31U, 28U, 31U, 30U, 31U, 30U,
		31U, 31U, 30U, 31U, 30U, 31U,
	};

	if (tomorrow == NULL || year < 1 || year > INT16_MAX ||
	    month < 1 || month > 12 || day < 1) {
		return -EINVAL;
	}
	const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	const int last_day = month_days[month - 1] + (month == 2 && leap ? 1 : 0);

	if (day > last_day) {
		return -EINVAL;
	}
	++day;
	if (day > last_day) {
		day = 1;
		++month;
		if (month > 12) {
			if (year == INT16_MAX) {
				return -ERANGE;
			}
			month = 1;
			++year;
		}
	}
	*tomorrow = (struct papermono_calendar_time) {
		.year = (int16_t)year,
		.month = (uint8_t)month,
		.day = (uint8_t)day,
	};
	return 0;
}

#endif /* PAPERMONO_STANDBY_DATE_H_ */
