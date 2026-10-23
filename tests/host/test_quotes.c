/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <papermono/quotes.h>

int main(void)
{
	const struct papermono_quote *first;
	const struct papermono_quote *same;
	const struct papermono_quote *next;

	assert(papermono_quote_count() >= 10U);
	first = papermono_quote_for_day(2026, 10, 23);
	same = papermono_quote_for_day(2026, 10, 23);
	assert(first != NULL && first == same);
	assert(first->text[0] != '\0' && first->book[0] != '\0');
	assert(first->author[0] != '\0');
	next = papermono_quote_for_day(2026, 10, 24);
	assert(next != first);
	/* Invalid input falls back to the first entry instead of reading OOB. */
	assert(papermono_quote_for_day(2026, 0, 0) != NULL);
	assert(papermono_quote_for_day(2026, 13, 40) != NULL);
	puts("quote daily rotation: PASS");
	return 0;
}
