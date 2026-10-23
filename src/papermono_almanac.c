/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <papermono/almanac.h>

/* Names follow the usual order: 小寒, 大寒, 立春 ... 冬至. */
static const char *const term_names[24] = {
	"小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
	"清明", "谷雨", "立夏", "小满", "芒种", "夏至",
	"小暑", "大暑", "立秋", "处暑", "白露", "秋分",
	"寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

/* Month/day pairs per term for the years the firmware ships tables for.
 * Source: published 2026 and 2027 solar-term times. Years outside this table
 * deliberately report "no data" instead of guessing with a formula.
 */
struct term_day {
	uint8_t month;
	uint8_t day;
};

struct term_year {
	uint16_t year;
	struct term_day terms[24];
};

static const struct term_year term_years[] = {
	{ 2026, {
		{1,5},{1,20},{2,4},{2,18},{3,5},{3,20},
		{4,5},{4,20},{5,5},{5,21},{6,5},{6,21},
		{7,7},{7,23},{8,7},{8,23},{9,7},{9,23},
		{10,8},{10,23},{11,7},{11,22},{12,7},{12,22} } },
	{ 2027, {
		{1,5},{1,20},{2,4},{2,19},{3,6},{3,21},
		{4,5},{4,20},{5,6},{5,21},{6,6},{6,21},
		{7,7},{7,23},{8,8},{8,23},{9,8},{9,23},
		{10,8},{10,23},{11,7},{11,22},{12,7},{12,22} } },
};

struct memorial_day {
	uint8_t month;
	uint8_t day;
	const char *name;
};

/* Fixed-date observances only. Lunar festivals need a lunar calendar table and
 * are intentionally absent rather than being approximated.
 */
static const struct memorial_day memorials[] = {
	{ 1, 1, "元旦" },
	{ 3, 12, "植树节" },
	{ 4, 22, "世界地球日" },
	{ 4, 23, "世界读书日" },
	{ 5, 1, "劳动节" },
	{ 9, 10, "教师节" },
	{ 10, 1, "国庆节" },
	{ 12, 25, "圣诞节" },
};

static bool leap_year(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int day_of_year(int year, int month, int day)
{
	static const uint8_t lengths[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	int total = 0;

	for (int index = 0; index < month - 1; ++index) {
		total += lengths[index] + (index == 1 && leap_year(year) ? 1 : 0);
	}
	return total + day;
}

static const struct term_year *find_year(int year)
{
	for (size_t index = 0U; index < sizeof(term_years) / sizeof(term_years[0]);
	     ++index) {
		if (term_years[index].year == year) {
			return &term_years[index];
		}
	}
	return NULL;
}

static void fill_term(struct papermono_almanac_term *out, int index,
	const struct term_day *day, int days_away)
{
	(void)strncpy(out->name, term_names[index], sizeof(out->name) - 1U);
	out->name[sizeof(out->name) - 1U] = '\0';
	out->month = day->month;
	out->day = day->day;
	out->days_away = (int16_t)days_away;
}

int papermono_almanac_lookup(int year, int month, int day,
	struct papermono_almanac_day *result)
{
	if (result == NULL || month < 1 || month > 12 || day < 1 || day > 31) {
		return -EINVAL;
	}
	memset(result, 0, sizeof(*result));
	for (size_t index = 0U;
	     index < sizeof(memorials) / sizeof(memorials[0]); ++index) {
		if (memorials[index].month == month && memorials[index].day == day) {
			result->memorial = memorials[index].name;
			break;
		}
	}

	const int today = day_of_year(year, month, day);
	const struct term_year *table = find_year(year);

	if (table != NULL) {
		for (int index = 0; index < 24; ++index) {
			const int term_day = day_of_year(year,
				table->terms[index].month, table->terms[index].day);

			if (term_day < today) {
				continue;
			}
			if (!result->term_valid) {
				result->term_valid = true;
				fill_term(&result->term, index, &table->terms[index],
					term_day - today);
			} else {
				result->next_term_valid = true;
				fill_term(&result->next_term, index,
					&table->terms[index], term_day - today);
				break;
			}
		}
	}
	if (result->term_valid && !result->next_term_valid) {
		/* The date is between 冬至 and year end: look at next year's table. */
		const struct term_year *next = find_year(year + 1);

		if (next != NULL) {
			result->next_term_valid = true;
			fill_term(&result->next_term, 0, &next->terms[0],
				day_of_year(year + 1, next->terms[0].month,
					next->terms[0].day) - today);
		}
	}
	if (!result->term_valid) {
		const struct term_year *next = find_year(year + 1);

		if (next != NULL) {
			/* No table for this year: still show the next tabulated term
			 * only when it belongs to next year, and say how far it is.
			 */
			result->term_valid = true;
			fill_term(&result->term, 0, &next->terms[0],
				day_of_year(year + 1, next->terms[0].month,
					next->terms[0].day) - today);
		}
	}
	return 0;
}
