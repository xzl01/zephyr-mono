/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <papermono/ui_layout.h>
#include <papermono/reader_layout.h>

static struct papermono_app_event tap(int x, int y)
{
	return (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = x, .y = y,
	};
}

int main(void)
{
	struct papermono_rect rect;
	uint16_t action;
	size_t slot;
	/* Check actual UI geometry, including all gutters and blank exterior. */
	for (size_t i = 0; i < 9; ++i) {
		assert(papermono_interaction_grid_cell_rect(&launcher_grid, i, &rect) == 0);
		struct papermono_app_event e = tap(rect.x + rect.width / 2, rect.y + rect.height / 2);
		assert(papermono_interaction_spaced_grid_index(&e, PAPERMONO_INTERACTION_TAP,
			&launcher_grid, &slot) == 0 && slot == i);
		assert(rect.x + rect.width <= PAPERMONO_UI_WIDTH);
		assert(rect.y + rect.height < UI_NAV_TOP);
		e = tap(rect.x + rect.width, rect.y + 1);
		assert(papermono_interaction_spaced_grid_index(&e, PAPERMONO_INTERACTION_TAP,
			&launcher_grid, &slot) == -ENOENT);
		e = tap(rect.x + 1, rect.y + rect.height);
		assert(papermono_interaction_spaced_grid_index(&e, PAPERMONO_INTERACTION_TAP,
			&launcher_grid, &slot) == -ENOENT);
	}
	for (size_t i = 0; i < 3; ++i) {
		struct papermono_app_event e = tap(i * 160 + 80, UI_NAV_TOP + 30);
		assert(papermono_interaction_route(&e, navigation_regions, 3, &action, NULL) == 0);
		assert(action == navigation_regions[i].action);
	}
	for (size_t i = 0; i < 3; ++i) {
		const struct papermono_rect *r = &home_regions[i].rect;
		struct papermono_app_event home = tap(r->x + r->width / 2, r->y + r->height / 2);
		assert(papermono_interaction_route(&home, home_regions, 3, &action, NULL) == 0);
		assert(action == home_regions[i].action);
		assert(r->y + r->height <= UI_NAV_TOP);
	}
	struct papermono_app_event e = tap(420, 30);
	const struct papermono_interaction_grid_layout *detail_grids[] = {
		&detail_list_grid, &detail_menu_grid,
	};
	for (size_t g = 0; g < 2; ++g) {
		const struct papermono_interaction_grid_layout *grid = detail_grids[g];
		for (size_t i = 0; i < grid->rows; ++i) {
			assert(papermono_interaction_grid_cell_rect(grid, i, &rect) == 0);
			e = tap(rect.x + rect.width / 2, rect.y + rect.height / 2);
			assert(papermono_interaction_spaced_grid_index(&e,
				PAPERMONO_INTERACTION_TAP, grid, &slot) == 0 && slot == i);
			assert(rect.y + rect.height < UI_DETAIL_FOOTER_TOP);
			e = tap(rect.x + 1, rect.y + rect.height);
			assert(papermono_interaction_spaced_grid_index(&e,
				PAPERMONO_INTERACTION_TAP, grid, &slot) == -ENOENT);
		}
	}
	assert(detail_confirm_rect.y + detail_confirm_rect.height < detail_cancel_rect.y);
	e = tap(240, (detail_confirm_rect.y + detail_confirm_rect.height + detail_cancel_rect.y) / 2);
	assert(!papermono_interaction_contains(&e, PAPERMONO_INTERACTION_TAP, &detail_confirm_rect));
	assert(!papermono_interaction_contains(&e, PAPERMONO_INTERACTION_TAP, &detail_cancel_rect));
	assert(detail_cancel_rect.y + detail_cancel_rect.height < UI_DETAIL_FOOTER_TOP);
	for (size_t i = 0; i < sizeof(detail_setting_rects) / sizeof(detail_setting_rects[0]); ++i) {
		const struct papermono_rect *r = &detail_setting_rects[i];
		assert(r->x >= 22 && r->x + r->width <= PAPERMONO_UI_WIDTH - 22);
		assert(r->height >= 56 && r->y + r->height < UI_DETAIL_FOOTER_TOP);
		if (i) assert(detail_setting_rects[i - 1].y + detail_setting_rects[i - 1].height <= r->y);
	}
	e = tap(420, 30);
	assert(papermono_interaction_route(&e, reader_regions, 6, &action, NULL) == 0);
	assert(action == LIBRARY_TOUCH_OPEN_TOC);
	e = tap(420, 768);
	assert(papermono_interaction_route(&e, reader_regions, 6, &action, NULL) == 0);
	assert(action == LIBRARY_TOUCH_BOOKMARK);
	e = tap(100, 768);
	assert(papermono_interaction_route(&e, reader_regions, 6, &action, NULL) == 0);
	assert(action == LIBRARY_TOUCH_OPEN_MENU);
	for (int density = 0; density < 3; ++density) {
		const struct papermono_reader_layout layout = papermono_reader_layout_for(density);
		const unsigned text_height = layout.lines *
			(PAPERMONO_READER_FONT_HEIGHT + layout.line_space) - layout.line_space;
		assert(text_height <= PAPERMONO_READER_BODY_HEIGHT);
		assert((layout.columns + PAPERMONO_TEXT_PUNCTUATION_OVERFLOW) * 24 <=
			PAPERMONO_UI_WIDTH - layout.margin * 2);
		assert(PAPERMONO_READER_BODY_TOP + text_height < PAPERMONO_UI_FOOTER_TOP);
	}
	puts("Field Notes UI hit targets and reader page fit: PASS");
	return 0;
}
