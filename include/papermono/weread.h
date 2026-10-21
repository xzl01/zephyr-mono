/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_WEREAD_H_
#define PAPERMONO_WEREAD_H_

#include <stdbool.h>
#include <stddef.h>

#include <papermono/personal_data.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_weread_status {
	char source[49];
	char synced_at[21];
	size_t count;
	size_t total;
	int last_error;
	bool truncated;
};

#define PAPERMONO_WEREAD_VISIBLE_BOOKS 5U

enum papermono_weread_view_mode {
	PAPERMONO_WEREAD_VIEW_LIST,
	PAPERMONO_WEREAD_VIEW_DETAIL,
};

enum papermono_weread_navigation {
	PAPERMONO_WEREAD_NAV_OPEN,
	PAPERMONO_WEREAD_NAV_PREVIOUS,
	PAPERMONO_WEREAD_NAV_NEXT,
	PAPERMONO_WEREAD_NAV_PAGE_PREVIOUS,
	PAPERMONO_WEREAD_NAV_PAGE_NEXT,
	PAPERMONO_WEREAD_NAV_BACK,
};

enum papermono_weread_navigation_result {
	PAPERMONO_WEREAD_NAVIGATION_NONE,
	PAPERMONO_WEREAD_NAVIGATION_REDRAW,
	PAPERMONO_WEREAD_NAVIGATION_ROUTER_BACK,
};

struct papermono_weread_view_state {
	size_t selected;
	enum papermono_weread_view_mode mode;
};

/** Reload the normalized, credential-free WeRead catalog from the TF card. */
int papermono_weread_reload(void);
void papermono_weread_get_status(struct papermono_weread_status *status);
int papermono_weread_get(size_t index, struct papermono_weread_book *book);

/** Pure view-state helpers shared by the LVGL page and host tests. */
void papermono_weread_view_reset(struct papermono_weread_view_state *state);
size_t papermono_weread_view_first(
	const struct papermono_weread_view_state *state);
int papermono_weread_view_navigate(struct papermono_weread_view_state *state,
	size_t book_count, enum papermono_weread_navigation navigation,
	enum papermono_weread_navigation_result *result);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_WEREAD_H_ */
