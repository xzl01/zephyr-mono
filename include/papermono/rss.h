/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_RSS_H_
#define PAPERMONO_RSS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_RSS_ITEM_MAX 12U
#define PAPERMONO_RSS_TITLE_MAX 127U
#define PAPERMONO_RSS_DATE_MAX 31U
/* Bodies live on the SD card; this is the largest one that may be stored. */
#define PAPERMONO_RSS_BODY_MAX (64U * 1024U)

struct papermono_rss_item_meta {
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	char published[PAPERMONO_RSS_DATE_MAX + 1U];
	uint32_t body_length;
};

/* RAM-resident index only: titles, dates and body lengths. The bodies are
 * read from the SD cache when an article is opened.
 */
struct papermono_rss_feed {
	bool valid;
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	size_t count;
	struct papermono_rss_item_meta items[PAPERMONO_RSS_ITEM_MAX];
	int64_t updated_at;
};

int papermono_rss_init(void);
void papermono_rss_get(struct papermono_rss_feed *out);
/* Refreshes on every news-page open, regardless of cache age or RTC validity.
 * Async: cached content stays readable; queued/in-flight requests coalesce.
 */
int papermono_rss_request(void);
/* 0 when idle, 1 while queued/fetching, a negative errno after the last failure. */
int papermono_rss_status(void);

/* Loads the full body of an item into `buffer` (NUL terminated). Returns the
 * number of bytes read, or a negative errno when the item or cache is missing.
 * The body is read from the SD cache, never kept in RAM between articles.
 */
int papermono_rss_body_load(size_t index, char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
