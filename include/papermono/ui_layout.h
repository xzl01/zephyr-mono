/* SPDX-License-Identifier: MIT */
#ifndef PAPERMONO_UI_LAYOUT_H_
#define PAPERMONO_UI_LAYOUT_H_

#include <papermono/interaction.h>

#define PAPERMONO_UI_WIDTH 480
#define PAPERMONO_UI_HEIGHT 800
#define PAPERMONO_UI_HEADER_HEIGHT 72
#define PAPERMONO_UI_FOOTER_TOP 742

#define UI_NAV_TOP 724
#define UI_DETAIL_FOOTER_TOP 724

/* Detail geometry is shared by rendering and event dispatch. */
static const struct papermono_interaction_grid_layout detail_list_grid = {
	.x = 22, .y = 148, .cell_width = 436, .cell_height = 104,
	.column_gap = 0, .row_gap = 8, .columns = 1, .rows = 5,
};
static const struct papermono_interaction_grid_layout detail_menu_grid = {
	.x = 22, .y = 148, .cell_width = 436, .cell_height = 76,
	.column_gap = 0, .row_gap = 4, .columns = 1, .rows = 7,
};
static const struct papermono_rect detail_confirm_rect = {22, 556, 436, 64};
static const struct papermono_rect detail_cancel_rect = {22, 634, 436, 64};
static const struct papermono_rect detail_sleep_rect = {22, 148, 436, 194};
static const struct papermono_rect detail_shutdown_rect = {22, 360, 436, 194};
static const struct papermono_rect detail_system_rect = {22, 148, 436, 104};
static const struct papermono_rect weread_import_rect = {64, 490, 352, 64};
static const struct papermono_rect library_rescan_rect = {22, 148, 436, 370};
enum detail_setting_index {
	DETAIL_SETTING_SLEEP, DETAIL_SETTING_TIME, DETAIL_SETTING_FRONTLIGHT,
	DETAIL_SETTING_WIFI, DETAIL_SETTING_BLE, DETAIL_SETTING_RESET,
	DETAIL_SETTING_POWER,
};
static const struct papermono_rect detail_setting_rects[] = {
	{22,174,436,56}, {22,230,436,56}, {22,286,436,100},
	{22,424,436,60}, {22,484,436,60}, {22,582,436,60}, {22,642,436,60},
};

static const struct papermono_interaction_region navigation_regions[] = {
	{ { 0, UI_NAV_TOP, 160, PAPERMONO_UI_HEIGHT - UI_NAV_TOP },
		PAPERMONO_APP_HOME, 0, PAPERMONO_INTERACTION_TAP },
	{ { 160, UI_NAV_TOP, 160, PAPERMONO_UI_HEIGHT - UI_NAV_TOP },
		PAPERMONO_APP_LAUNCHER, 0, PAPERMONO_INTERACTION_TAP },
	{ { 320, UI_NAV_TOP, 160, PAPERMONO_UI_HEIGHT - UI_NAV_TOP },
		PAPERMONO_APP_SETTINGS, 0, PAPERMONO_INTERACTION_TAP },
};

enum home_region_index {
	HOME_REGION_READER,
	HOME_REGION_CALENDAR,
	HOME_REGION_TASKS,
};

static const struct papermono_interaction_region home_regions[] = {
	[HOME_REGION_READER] = { { 22, 220, 436, 216 },
		PAPERMONO_APP_LIBRARY, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_CALENDAR] = { { 22, 436, 436, 120 },
		PAPERMONO_APP_CALENDAR, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_TASKS] = { { 22, 556, 436, 168 },
		PAPERMONO_APP_TASKS, 0, PAPERMONO_INTERACTION_TAP },
};

static const struct papermono_interaction_grid_layout launcher_grid = {
	.x = 22,
	.y = 124,
	.cell_width = 136,
	.cell_height = 180,
	.column_gap = 14,
	.row_gap = 16,
	.columns = 3,
	.rows = 3,
};

enum library_touch_action {
	LIBRARY_TOUCH_PREVIOUS_PAGE = 1,
	LIBRARY_TOUCH_NEXT_PAGE,
	LIBRARY_TOUCH_OPEN_MENU,
	LIBRARY_TOUCH_OPEN_TOC,
	LIBRARY_TOUCH_BOOKMARK,
};

static const struct papermono_interaction_region reader_regions[] = {
	{ { 380, 12, 78, 40 }, LIBRARY_TOUCH_OPEN_TOC, 0, PAPERMONO_INTERACTION_TAP },
	{ { 360, PAPERMONO_UI_FOOTER_TOP, 120, PAPERMONO_UI_HEIGHT - PAPERMONO_UI_FOOTER_TOP },
		LIBRARY_TOUCH_BOOKMARK, 0, PAPERMONO_INTERACTION_TAP },
	{ { 0, PAPERMONO_UI_FOOTER_TOP, 360, PAPERMONO_UI_HEIGHT - PAPERMONO_UI_FOOTER_TOP },
		LIBRARY_TOUCH_OPEN_MENU, 0, PAPERMONO_INTERACTION_TAP },
	{ { 0, PAPERMONO_UI_HEADER_HEIGHT, 160, PAPERMONO_UI_FOOTER_TOP - PAPERMONO_UI_HEADER_HEIGHT },
		LIBRARY_TOUCH_PREVIOUS_PAGE, 0, PAPERMONO_INTERACTION_TAP },
	{ { 320, PAPERMONO_UI_HEADER_HEIGHT, 160, PAPERMONO_UI_FOOTER_TOP - PAPERMONO_UI_HEADER_HEIGHT },
		LIBRARY_TOUCH_NEXT_PAGE, 0, PAPERMONO_INTERACTION_TAP },
	{ { 160, PAPERMONO_UI_HEADER_HEIGHT, 160, PAPERMONO_UI_FOOTER_TOP - PAPERMONO_UI_HEADER_HEIGHT },
		LIBRARY_TOUCH_OPEN_MENU, 0, PAPERMONO_INTERACTION_TAP },
};

#endif
