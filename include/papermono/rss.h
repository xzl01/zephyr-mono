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
/* Full digest bodies run to ~3.5k CJK characters (about 10.5 KB of UTF-8);
 * 511 bytes only kept the first paragraph, which looked like a one-page
 * article. Keep room for the whole thing. */
#define PAPERMONO_RSS_BODY_MAX 12000U
#define PAPERMONO_RSS_DATE_MAX 31U

struct papermono_rss_item {
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	/* Plain text: tags stripped, entities decoded, whitespace collapsed. */
	char body[PAPERMONO_RSS_BODY_MAX + 1U];
	char published[PAPERMONO_RSS_DATE_MAX + 1U];
};

struct papermono_rss_feed {
	bool valid;
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	size_t count;
	struct papermono_rss_item items[PAPERMONO_RSS_ITEM_MAX];
	/* Wall-clock seconds; 0 when the RTC/SNTP clock is not usable. */
	int64_t updated_at;
};

int papermono_rss_init(void);
/* Copies the latest snapshot; always safe to call from the UI thread. */
void papermono_rss_get(struct papermono_rss_feed *out);
/* Asks the worker for an immediate refresh; no-op while one is running. */
int papermono_rss_request(void);

#ifdef __cplusplus
}
#endif

#endif
