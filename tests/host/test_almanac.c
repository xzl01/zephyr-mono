/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <papermono/almanac.h>

int main(void)
{
	struct papermono_almanac_day day;

	/* 2026-10-23 is the published 霜降 date. */
	assert(papermono_almanac_lookup(2026, 10, 23, &day) == 0);
	assert(day.term_valid && strcmp(day.term.name, "霜降") == 0);
	assert(day.term.days_away == 0);
	assert(day.next_term_valid && strcmp(day.next_term.name, "立冬") == 0);
	assert(day.next_term.days_away == 15);
	assert(day.memorial == NULL);

	/* Before the term, the same entry is reported with its distance. */
	assert(papermono_almanac_lookup(2026, 10, 20, &day) == 0);
	assert(strcmp(day.term.name, "霜降") == 0);
	assert(day.term.days_away == 3);

	/* Fixed-date observance and nearest term coexist. */
	assert(papermono_almanac_lookup(2026, 4, 23, &day) == 0);
	assert(day.memorial != NULL && strcmp(day.memorial, "世界读书日") == 0);
	assert(day.term_valid && strcmp(day.term.name, "立夏") == 0);

	/* 2027-02-19 is 雨水 in the shipped table. */
	assert(papermono_almanac_lookup(2027, 2, 19, &day) == 0);
	assert(strcmp(day.term.name, "雨水") == 0 && day.term.days_away == 0);

	/* Outside the tabulated years the term is withheld, not guessed. */
	assert(papermono_almanac_lookup(2035, 6, 1, &day) == 0);
	assert(!day.term_valid && !day.next_term_valid);

	assert(papermono_almanac_lookup(2026, 13, 1, &day) == -EINVAL);
	assert(papermono_almanac_lookup(2026, 0, 1, &day) == -EINVAL);
	puts("almanac terms and observances: PASS");
	return 0;
}
