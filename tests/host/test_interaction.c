/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <papermono/interaction.h>

static void test_explicit_input_source(void)
{
	const struct papermono_app_event button = {
		.type = PAPERMONO_APP_EVENT_NEXT,
		.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON,
		.x = 100,
		.y = 100,
	};
	const struct papermono_app_event legacy_button = {
		.type = PAPERMONO_APP_EVENT_PREVIOUS,
		.x = -1,
		.y = -1,
	};
	const struct papermono_app_event touch = {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = 100,
		.y = 100,
	};

	assert(papermono_app_event_is_button(&button));
	assert(!papermono_app_event_is_touch(&button));
	assert(papermono_app_event_is_button(&legacy_button));
	assert(papermono_app_event_is_touch(&touch));
	assert(papermono_app_event_is_back_navigation(&(struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_BACK,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
	}));
	assert(papermono_app_event_is_back_navigation(&(struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_PREVIOUS,
		.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON,
	}));
	assert(!papermono_app_event_is_back_navigation(&touch));
	assert(!papermono_app_event_is_back_navigation(NULL));
}

static void test_region_routing(void)
{
	static const struct papermono_interaction_region regions[] = {
		{ { 0, 0, 100, 100 }, 10U, 1,
			PAPERMONO_INTERACTION_TAP },
		{ { 100, 0, 100, 100 }, 20U, 2,
			PAPERMONO_INTERACTION_LONG_PRESS },
	};
	struct papermono_app_event event = {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = 99,
		.y = 99,
	};
	uint16_t action = 0U;
	int16_t value = 0;

	assert(papermono_interaction_route(&event, regions, 2U, &action,
		&value) == 0);
	assert(action == 10U && value == 1);
	event.x = 100;
	assert(papermono_interaction_route(&event, regions, 2U, &action,
		&value) == -ENOENT);
	event.type = PAPERMONO_APP_EVENT_LONG_PRESS;
	assert(papermono_interaction_route(&event, regions, 2U, &action,
		&value) == 0);
	assert(action == 20U && value == 2);
}

static void test_grid_mapping(void)
{
	const struct papermono_rect grid = { 0, 80, 480, 640 };
	struct papermono_app_event event = {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = 300,
		.y = 350,
	};
	size_t index;

	assert(papermono_interaction_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &grid, 2U, 5U, &index) == 0);
	assert(index == 5U);
	event.x = 479;
	event.y = 719;
	assert(papermono_interaction_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &grid, 2U, 5U, &index) == 0);
	assert(index == 9U);
	event.y = 720;
	assert(papermono_interaction_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &grid, 2U, 5U, &index) == -ENOENT);
}

static void test_spaced_grid_mapping(void)
{
	const struct papermono_interaction_grid_layout layout = {
		.x = 18,
		.y = 112,
		.cell_width = 148,
		.cell_height = 210,
		.column_gap = 0,
		.row_gap = 0,
		.columns = 3,
		.rows = 3,
	};
	struct papermono_app_event event = {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
	};
	struct papermono_rect rect;
	size_t index;

	assert(papermono_interaction_grid_cell_rect(&layout, 0U, &rect) == 0);
	assert(rect.x == 18 && rect.y == 112 && rect.width == 148 &&
		rect.height == 210);
	assert(papermono_interaction_grid_cell_rect(&layout, 8U, &rect) == 0);
	assert(rect.x == 314 && rect.y == 532 && rect.width == 148 &&
		rect.height == 210);
	assert(papermono_interaction_grid_cell_rect(&layout, 9U, &rect) ==
		-ERANGE);

	/* The outer bounds cover x=18..461 and y=112..741. */
	event.x = 18;
	event.y = 112;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 0U);
	event.x = 165;
	event.y = 321;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 0U);

	/* With shared borders and no gutter, the next pixel belongs to the cell on
	 * the right or below. Their intersection therefore resolves to cell 4.
	 */
	event.x = 166;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 1U);
	event.x = 18;
	event.y = 322;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 3U);
	event.x = 166;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 4U);

	/* Each page exposes exactly nine row-major targets. */
	for (size_t expected = 0U; expected < 9U; ++expected) {
		const size_t column = expected % 3U;
		const size_t row = expected / 3U;

		event.x = (int16_t)(18 + column * 148U + 74U);
		event.y = (int16_t)(112 + row * 210U + 105U);
		assert(papermono_interaction_spaced_grid_index(&event,
			PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
		assert(index == expected);
	}

	event.x = 461;
	event.y = 741;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == 0);
	assert(index == 8U);

	/* One pixel outside any side must not activate a neighboring page item. */
	event.x = 17;
	event.y = 112;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == -ENOENT);
	event.x = 462;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == -ENOENT);
	event.x = 18;
	event.y = 111;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == -ENOENT);
	event.y = 742;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &layout, &index) == -ENOENT);
}

static void test_remote_spaced_grids(void)
{
	const struct papermono_interaction_grid_layout tabs = {
		.x = 22,
		.y = 212,
		.cell_width = 140,
		.cell_height = 64,
		.column_gap = 8,
		.row_gap = 0,
		.columns = 3,
		.rows = 1,
	};
	const struct papermono_interaction_grid_layout actions = {
		.x = 22,
		.y = 296,
		.cell_width = 214,
		.cell_height = 112,
		.column_gap = 8,
		.row_gap = 18,
		.columns = 2,
		.rows = 3,
	};
	struct papermono_app_event event = {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
	};
	size_t index;

	event.x = 161;
	event.y = 230;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &tabs, &index) == 0);
	assert(index == 0U);
	event.x = 162;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &tabs, &index) == -ENOENT);
	event.x = 170;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &tabs, &index) == 0);
	assert(index == 1U);

	event.x = 235;
	event.y = 407;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &actions, &index) == 0);
	assert(index == 0U);
	event.x = 236;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &actions, &index) == -ENOENT);
	event.x = 244;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &actions, &index) == 0);
	assert(index == 1U);
	event.x = 30;
	event.y = 408;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &actions, &index) == -ENOENT);
	event.y = 426;
	assert(papermono_interaction_spaced_grid_index(&event,
		PAPERMONO_INTERACTION_TAP, &actions, &index) == 0);
	assert(index == 2U);
}

int main(void)
{
	test_explicit_input_source();
	test_region_routing();
	test_grid_mapping();
	test_spaced_grid_mapping();
	test_remote_spaced_grids();
	puts("interaction routing tests: PASS");
	return 0;
}
