/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>

#include <papermono/weread.h>

void papermono_weread_view_reset(struct papermono_weread_view_state *state)
{
	if (state == NULL) {
		return;
	}
	*state = (struct papermono_weread_view_state) {
		.mode = PAPERMONO_WEREAD_VIEW_LIST,
	};
}

size_t papermono_weread_view_first(
	const struct papermono_weread_view_state *state)
{
	return state == NULL ? 0U :
		(state->selected / PAPERMONO_WEREAD_VISIBLE_BOOKS) *
		PAPERMONO_WEREAD_VISIBLE_BOOKS;
}

int papermono_weread_view_navigate(struct papermono_weread_view_state *state,
	size_t book_count, enum papermono_weread_navigation navigation,
	enum papermono_weread_navigation_result *result)
{
	if (state == NULL || result == NULL ||
	    navigation > PAPERMONO_WEREAD_NAV_BACK) {
		return -EINVAL;
	}
	*result = PAPERMONO_WEREAD_NAVIGATION_NONE;
	if (book_count == 0U) {
		state->selected = 0U;
	} else if (state->selected >= book_count) {
		state->selected = book_count - 1U;
	}

	if (navigation == PAPERMONO_WEREAD_NAV_BACK) {
		if (state->mode == PAPERMONO_WEREAD_VIEW_DETAIL) {
			state->mode = PAPERMONO_WEREAD_VIEW_LIST;
			*result = PAPERMONO_WEREAD_NAVIGATION_REDRAW;
		} else {
			*result = PAPERMONO_WEREAD_NAVIGATION_ROUTER_BACK;
		}
		return 0;
	}
	if (navigation == PAPERMONO_WEREAD_NAV_OPEN) {
		if (book_count > 0U &&
		    state->mode == PAPERMONO_WEREAD_VIEW_LIST) {
			state->mode = PAPERMONO_WEREAD_VIEW_DETAIL;
			*result = PAPERMONO_WEREAD_NAVIGATION_REDRAW;
		}
		return 0;
	}
	if (book_count == 0U) {
		return 0;
	}

	size_t selected = state->selected;

	switch (navigation) {
	case PAPERMONO_WEREAD_NAV_PREVIOUS:
		if (selected > 0U) {
			--selected;
		}
		break;
	case PAPERMONO_WEREAD_NAV_NEXT:
		if (selected + 1U < book_count) {
			++selected;
		}
		break;
	case PAPERMONO_WEREAD_NAV_PAGE_PREVIOUS:
		selected = selected >= PAPERMONO_WEREAD_VISIBLE_BOOKS ?
			selected - PAPERMONO_WEREAD_VISIBLE_BOOKS : 0U;
		break;
	case PAPERMONO_WEREAD_NAV_PAGE_NEXT:
		if (selected + PAPERMONO_WEREAD_VISIBLE_BOOKS < book_count) {
			selected += PAPERMONO_WEREAD_VISIBLE_BOOKS;
		} else {
			selected = book_count - 1U;
		}
		break;
	case PAPERMONO_WEREAD_NAV_OPEN:
	case PAPERMONO_WEREAD_NAV_BACK:
	default:
		return -EINVAL;
	}
	if (selected != state->selected) {
		state->selected = selected;
		*result = PAPERMONO_WEREAD_NAVIGATION_REDRAW;
	}
	return 0;
}
