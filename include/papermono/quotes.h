/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_QUOTES_H_
#define PAPERMONO_QUOTES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in excerpt library for the standby page. Entries stay in the public
 * domain or are short attributed quotations; the book title is only used to
 * offer "书架里有这本书" when the same title exists locally.
 */
struct papermono_quote {
	const char *text;
	const char *book;
	const char *author;
};

/* Deterministic daily rotation: the same day always yields the same entry. */
const struct papermono_quote *papermono_quote_for_day(int year, int month,
	int day);
size_t papermono_quote_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_QUOTES_H_ */
