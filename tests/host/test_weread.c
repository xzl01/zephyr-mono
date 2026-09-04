/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <papermono/weread.h>

int main(void)
{
	struct papermono_weread_view_state state;
	enum papermono_weread_navigation_result result;

	papermono_weread_view_reset(&state);
	assert(state.mode == PAPERMONO_WEREAD_VIEW_LIST);
	assert(state.selected == 0U);
	assert(papermono_weread_view_first(&state) == 0U);

	/* An ordinary right swipe/previous action changes selection; only the
	 * explicit edge-back gesture leaves the application at the list root.
	 */
	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_PREVIOUS, &result) == 0);
	assert(result == PAPERMONO_WEREAD_NAVIGATION_NONE);
	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_BACK, &result) == 0);
	assert(result == PAPERMONO_WEREAD_NAVIGATION_ROUTER_BACK);

	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_PAGE_NEXT, &result) == 0);
	assert(result == PAPERMONO_WEREAD_NAVIGATION_REDRAW);
	assert(state.selected == 5U && papermono_weread_view_first(&state) == 5U);
	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_OPEN, &result) == 0);
	assert(state.mode == PAPERMONO_WEREAD_VIEW_DETAIL);
	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_NEXT, &result) == 0);
	assert(state.selected == 6U);
	assert(papermono_weread_view_navigate(&state, 12U,
		PAPERMONO_WEREAD_NAV_BACK, &result) == 0);
	assert(state.mode == PAPERMONO_WEREAD_VIEW_LIST);
	assert(result == PAPERMONO_WEREAD_NAVIGATION_REDRAW);

	state.selected = 99U;
	assert(papermono_weread_view_navigate(&state, 3U,
		PAPERMONO_WEREAD_NAV_NEXT, &result) == 0);
	assert(state.selected == 2U);
	assert(papermono_weread_view_navigate(&state, 0U,
		PAPERMONO_WEREAD_NAV_OPEN, &result) == 0);
	assert(state.selected == 0U && state.mode == PAPERMONO_WEREAD_VIEW_LIST);
	assert(papermono_weread_view_navigate(NULL, 0U,
		PAPERMONO_WEREAD_NAV_BACK, &result) == -EINVAL);

	puts("WeRead view navigation tests: PASS");
	return 0;
}
