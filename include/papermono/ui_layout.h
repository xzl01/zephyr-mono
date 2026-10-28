/* SPDX-License-Identifier: GPL-3.0-only */
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
/* System page: tap target that opens the TF format confirmation when the card
 * carries no filesystem this firmware can mount (exFAT/GPT out of the box). */
static const struct papermono_rect system_storage_rect = {22, 680, 436, 44};
/* On-device Wi-Fi: displayed rectangles are also the only touch targets. */
static const struct papermono_rect wifi_scan_rect = {22, 218, 138, 50};
static const struct papermono_rect wifi_reconnect_rect = {170, 218, 138, 50};
static const struct papermono_rect wifi_forget_rect = {318, 218, 140, 50};
static const struct papermono_interaction_grid_layout wifi_network_grid = {
	.x = 22, .y = 306, .cell_width = 436, .cell_height = 76,
	.column_gap = 0, .row_gap = 4, .columns = 1, .rows = 4,
};
static const struct papermono_rect wifi_previous_rect = {22, 646, 206, 54};
static const struct papermono_rect wifi_next_rect = {252, 646, 206, 54};
static const struct papermono_rect wifi_connect_rect = {22, 292, 436, 54};
/* News skips the global page header: content starts at the top margin, and
 * the article viewport is sized to whole reading lines (line height + 4 px
 * line space) so a page turn never leaves a half-clipped last line.
 */
#define NEWS_MARGIN_X 22
#define NEWS_TEXT_WIDTH 436
#define NEWS_TITLE_Y 24
#define NEWS_BODY_TOP 92
#define NEWS_BODY_BOTTOM 744
#define NEWS_FOOTER_Y 756
#define NEWS_LINE_SPACE 4
/* The source strip replaces the old feed-title line; the list's eight rows
 * and metadata band retain their existing positions. */
static const struct papermono_rect news_sources_rect = {22, 12, 436, 44};
static const struct papermono_interaction_grid_layout news_list_grid = {
	.x = 22, .y = 80, .cell_width = 436, .cell_height = 78,
	.column_gap = 0, .row_gap = 4, .columns = 1, .rows = 8,
};
/* Maximum body area; render_news_detail() shrinks it to an exact number of
 * line strides. Touch handling only needs the enclosing rectangle.
 */
static const struct papermono_rect news_body_rect = {22, NEWS_BODY_TOP, 436,
	NEWS_BODY_BOTTOM - NEWS_BODY_TOP};

enum detail_setting_index {
	DETAIL_SETTING_SLEEP, DETAIL_SETTING_TIME, DETAIL_SETTING_FRONTLIGHT,
	DETAIL_SETTING_SUMMARY, DETAIL_SETTING_WIFI, DETAIL_SETTING_BLE,
	DETAIL_SETTING_RESET, DETAIL_SETTING_POWER,
};
static const struct papermono_rect detail_setting_rects[] = {
	{22,162,436,56}, {22,218,436,56}, {22,274,436,88},
	{22,362,436,56}, {22,456,436,56}, {22,512,436,56},
	{22,608,436,56}, {22,664,436,56},
};
/* The settings page and its event dispatch index this table by
 * enum detail_setting_index, so a missing or extra row silently shifts every
 * hit region below it.
 */
_Static_assert(sizeof(detail_setting_rects) / sizeof(detail_setting_rects[0]) ==
		DETAIL_SETTING_POWER + 1U,
	"detail_setting_rects must have one row per detail_setting_index");

/* Keep the five frontlight markers below both title and value, inside the
 * same row that receives taps and swipes. Width is the first-to-last centre.
 */
static inline struct papermono_rect papermono_ui_frontlight_track(void)
{
	const struct papermono_rect *row =
		&detail_setting_rects[DETAIL_SETTING_FRONTLIGHT];

	return (struct papermono_rect) {
		row->x + 18, row->y + 68, row->width - 52, 1,
	};
}

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
