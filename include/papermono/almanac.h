/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_ALMANAC_H_
#define PAPERMONO_ALMANAC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 二十四节气与常见公历纪念日。数据是随固件打包的固定表：节气按已发布的
 * 2026/2027 时刻表收录，表外年份明确返回“无数据”，不用公式现算，避免差一天
 * 的节气信息看起来像真的。
 */
#define PAPERMONO_ALMANAC_TERM_MAX 16
#define PAPERMONO_ALMANAC_MEMORIAL_MAX 16

struct papermono_almanac_term {
	char name[PAPERMONO_ALMANAC_TERM_MAX];
	uint16_t month;
	uint16_t day;
	/* Days from the queried date to this term (0 = today). */
	int16_t days_away;
};

struct papermono_almanac_day {
	/* Nearest term on/after the date, or false when the year is not tabulated. */
	bool term_valid;
	struct papermono_almanac_term term;
	/* Nearest following term, for "距 X N 天". */
	bool next_term_valid;
	struct papermono_almanac_term next_term;
	/* Fixed-date observance matching the date exactly, if any. */
	const char *memorial;
};

/* Fills the fields for a local calendar date. Invalid dates return -EINVAL. */
int papermono_almanac_lookup(int year, int month, int day,
	struct papermono_almanac_day *result);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ALMANAC_H_ */
