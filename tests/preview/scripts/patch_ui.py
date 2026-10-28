#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Append event assertions and fixtures; never rewrite the production snapshot."""
from pathlib import Path
import sys

GLUE = r'''

/* Native preview test/fixture glue. UI above is a byte-for-byte snapshot.
 * Same translation unit grants access to actual private event/render/close
 * functions; tests do not replace production handlers or validation code. */
enum preview_wifi_operation {
	PREVIEW_WIFI_SCAN, PREVIEW_WIFI_SAVED, PREVIEW_WIFI_FORGET,
	PREVIEW_WIFI_CONNECT, PREVIEW_WIFI_RESET, PREVIEW_WIFI_OPERATIONS
};
int preview_init(void);
int preview_wifi_fixture(const char *name);
extern bool preview_sync_active;
void preview_wifi_probe_reset(void);
void preview_wifi_result(unsigned operation, int result);
unsigned preview_wifi_calls(unsigned operation, bool accepted);
void preview_wifi_expect_connect(const struct papermono_wifi_network *network,
	const char *password);
bool preview_wifi_connect_matched(void);
void preview_wifi_set_state(const struct papermono_wifi_state *state);
void preview_display_result(int result);
void preview_fs_mount_result(int result);
void preview_hid_set_state(const struct papermono_hid_state *state);
void preview_rss_set_index(const struct papermono_rss_feed *feed);
void preview_rss_request_result_set(int result);
void preview_rss_status_set(int status);
void preview_rss_body_fail_set(bool fail);
unsigned preview_rss_requests(void);
unsigned preview_rss_body_loads(void);
void preview_rss_body_text_set(const char *text);
void preview_rss_defaults(void);
void preview_rss_set_source_index(size_t source, const struct papermono_rss_feed *feed);
void preview_rss_source_status_set(size_t source, int status);
void preview_rss_source_body_set(size_t source, const char *text);

static const char preview_rss_table_body[] =
	"表前说明：以下数据仅用于界面测试。\n\n"
	PAPERMONO_RSS_TABLE_BEGIN
	PAPERMONO_RSS_TABLE_HEADER "项目\t现价\t变化\n"
	PAPERMONO_RSS_TABLE_ROW "黄金\t1234.56\t+1.20%\n"
	PAPERMONO_RSS_TABLE_ROW "白银\t12.34\t-0.50%\n"
	PAPERMONO_RSS_TABLE_ROW "铂金\t456.78\t持平\n"
	PAPERMONO_RSS_TABLE_END
	"\n表后说明：全文按原顺序保留。";
static const char preview_rss_wide_table_body[] =
	"宽表按列分组，首列保留项目名称。\n\n"
	PAPERMONO_RSS_TABLE_BEGIN
	PAPERMONO_RSS_TABLE_HEADER "项目\t现价\t变化\t最高\t最低\t单位\n"
	PAPERMONO_RSS_TABLE_ROW "黄金\t1234.56\t+1.20%\t1250.00\t1200.00\t测试单位\n"
	PAPERMONO_RSS_TABLE_ROW "白银\t12.34\t-0.50%\t13.00\t12.00\t测试单位\n"
	PAPERMONO_RSS_TABLE_END
	"\n表后说明：后面的正文不会丢失。";
static char preview_rss_long_table_body[8192];
static char preview_rss_overflow_table_body[24000];
static void preview_make_long_table(void)
{
	size_t used = (size_t)snprintf(preview_rss_long_table_body,
		sizeof(preview_rss_long_table_body),
		PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_HEADER "项目\t完整说明\t数值\n"
		PAPERMONO_RSS_TABLE_ROW "长说明\t");
	for (size_t i = 0U; i < 40U; ++i) {
		used += (size_t)snprintf(preview_rss_long_table_body + used,
			sizeof(preview_rss_long_table_body) - used, "长单元格不截断");
	}
	(void)snprintf(preview_rss_long_table_body + used,
		sizeof(preview_rss_long_table_body) - used,
		"长格结束\t999.99\n" PAPERMONO_RSS_TABLE_END "\n长表后的正文。\n");
}
static void preview_make_dense_table(void)
{
	size_t used = (size_t)snprintf(preview_rss_long_table_body,
		sizeof(preview_rss_long_table_body),
		PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_HEADER "项目\t数值\t单位\n");
	for (size_t i = 1U; i <= 20U; ++i) {
		used += (size_t)snprintf(preview_rss_long_table_body + used,
			sizeof(preview_rss_long_table_body) - used,
			PAPERMONO_RSS_TABLE_ROW "行%02zu\t100.%02zu\t测试\n", i, i);
	}
	(void)snprintf(preview_rss_long_table_body + used,
		sizeof(preview_rss_long_table_body) - used, PAPERMONO_RSS_TABLE_END);
}

#define PREVIEW_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "preview assertion failed: line %d: %s\n", \
			__LINE__, #condition); \
		return -EINVAL; \
	} \
} while (0)

static bool preview_has_text(lv_obj_t *root, const char *text)
{
	if (lv_obj_check_type(root, &lv_label_class) &&
	    strstr(lv_label_get_text(root), text) != NULL) {
		return true;
	}
	for (uint32_t i = 0U; i < lv_obj_get_child_count(root); ++i) {
		if (preview_has_text(lv_obj_get_child(root, i), text)) return true;
	}
	return false;
}

static bool preview_zeroed(const void *memory, size_t length)
{
	const unsigned char *bytes = memory;
	for (size_t i = 0; i < length; ++i) if (bytes[i] != 0) return false;
	return true;
}

static bool preview_draft_erased(void)
{
	return wifi_ui.mode == WIFI_UI_LIST && !atomic_get(&wifi_keyboard_active) &&
		preview_zeroed(&wifi_ui.input, sizeof(wifi_ui.input)) &&
		preview_zeroed(wifi_ui.display_text, sizeof(wifi_ui.display_text));
}

static struct papermono_app_event preview_tap(const struct papermono_rect *rect)
{
	return (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_TAP,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = rect->x + rect->width / 2,
		.y = rect->y + rect->height / 2,
		.input_generation = (uint32_t)atomic_get(&wifi_input_generation),
	};
}

static int preview_wifi_tap(const struct papermono_rect *rect,
	struct papermono_app_command *command)
{
	struct papermono_app_event event = preview_tap(rect);
	*command = (struct papermono_app_command) {0};
	return wifi_event(NULL, &event, command);
}

static int preview_type_demo(const char *text)
{
	/* Exercise real keyboard hit testing and wifi_event, not append directly.
	 * Demo values intentionally use the lower-case layer here. */
	for (const char *c = text; *c; ++c) {
		bool found = false;
		for (size_t i = 0; i < PAPERMONO_WIFI_INPUT_CHARACTER_KEYS; ++i) {
			struct papermono_wifi_input_key key;
			PREVIEW_CHECK(papermono_wifi_input_key_at(&wifi_ui.input, i, &key) == 0);
			if (key.character != *c) continue;
			struct papermono_rect rect = {key.x, key.y, key.width, key.height};
			struct papermono_app_command command;
			PREVIEW_CHECK(preview_wifi_tap(&rect, &command) == 0);
			PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_REDRAW);
			found = true;
			break;
		}
		PREVIEW_CHECK(found);
	}
	return 0;
}

static int preview_select_saved(struct papermono_rect *row)
{
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(papermono_interaction_grid_cell_rect(&wifi_network_grid, 0, row) == 0);
	struct papermono_app_command command;
	PREVIEW_CHECK(preview_wifi_tap(row, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_REDRAW);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_PASSWORD);
	PREVIEW_CHECK(atomic_get(&wifi_keyboard_active));
	PREVIEW_CHECK(wifi_ui.input.length == 0 && !wifi_ui.input.visible);
	PREVIEW_CHECK(strcmp(wifi_ui.selected.ssid, wifi_ui.shown.saved_ssid) == 0);
	return 0;
}

int preview_run_assertions(void)
{
	struct papermono_app_command command;
	struct papermono_rect row;
	struct papermono_wifi_state original, state;
	PREVIEW_CHECK(preview_wifi_fixture("wifi-list") == 0);
	preview_wifi_probe_reset();
	preview_display_result(0);
	papermono_content_server_get_wifi_state(&original);

	/* Settings entry only navigates; opening Wi-Fi scans, never clears the
	 * saved SSID/credentials or invokes the legacy reset/forget APIs. */
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_SETTINGS) == 0);
	struct papermono_app_event event = preview_tap(&detail_setting_rects[DETAIL_SETTING_WIFI]);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(settings_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_OPEN && command.target == PAPERMONO_APP_WIFI);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_RESET, false) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, false) == 0);
	PREVIEW_CHECK(process_event(&event) == 0 && app_router.current == PAPERMONO_APP_WIFI);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SCAN, false) == 1);
	papermono_content_server_get_wifi_state(&state);
	PREVIEW_CHECK(state.configured && strcmp(state.saved_ssid, original.saved_ssid) == 0);

	PREVIEW_CHECK(preview_select_saved(&row) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SAVED, false) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_CONNECT, false) == 0);
	PREVIEW_CHECK(atomic_get(&wifi_layout_transition));
	PREVIEW_CHECK(preview_type_demo("demo123") == 0);
	/* Populate visible display storage too, so later erasure is not vacuous. */
	wifi_ui.input.visible = true;
	state = original;
	++state.scan_generation;
	state.connection = PAPERMONO_WIFI_DISCONNECTED;
	preview_wifi_set_state(&state);
	for (unsigned i = 0; i < 2; ++i) {
		event = (struct papermono_app_event) {
			.type = i == 0 ? PAPERMONO_APP_EVENT_REFRESH : PAPERMONO_APP_EVENT_CONTENT_CHANGED,
			.source = PAPERMONO_APP_EVENT_SOURCE_SYSTEM,
		};
		PREVIEW_CHECK(process_event(&event) == 0);
		PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_PASSWORD && wifi_ui.input.length == 7);
		PREVIEW_CHECK(strcmp(wifi_ui.input.text, "demo123") == 0);
		PREVIEW_CHECK(strcmp(wifi_ui.display_text, "demo123") == 0);
	}

	/* Production validates length in content_server_connect_wifi, not in
	 * wifi_event. Inject that API's documented rejection, and distinguish an
	 * API attempt from an accepted request: this is NOT backend validation. */
	preview_wifi_result(PREVIEW_WIFI_CONNECT, -EINVAL);
	preview_wifi_expect_connect(&original.networks[0], "demo123");
	PREVIEW_CHECK(preview_wifi_tap(&wifi_connect_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_connect_matched());
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_CONNECT, false) == 1);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_CONNECT, true) == 0);
	PREVIEW_CHECK(wifi_ui.last_error == -EINVAL && wifi_ui.mode == WIFI_UI_PASSWORD);
	PREVIEW_CHECK(strcmp(wifi_ui.input.text, "demo123") == 0);
	PREVIEW_CHECK(preview_type_demo("4") == 0);
	preview_wifi_expect_connect(&original.networks[0], "demo1234");
	preview_wifi_result(PREVIEW_WIFI_CONNECT, -EBUSY);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_connect_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_connect_matched() && wifi_ui.last_error == -EBUSY);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_PASSWORD && wifi_ui.input.length == 8);
	/* Saturated-keyboard latch must prevent even an API attempt. */
	atomic_set(&wifi_keyboard_overflow, 1);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_connect_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.last_error == -EOVERFLOW);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_CONNECT, false) == 2);
	atomic_set(&wifi_keyboard_overflow, 0);
	preview_wifi_result(PREVIEW_WIFI_CONNECT, 0);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_connect_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_connect_matched());
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_CONNECT, true) == 1);
	PREVIEW_CHECK(preview_draft_erased());

	/* Both cancellation paths and the registered on_close hook erase the
	 * entire input model plus visible display storage, not just byte zero. */
	for (unsigned cancel = 0; cancel < 2; ++cancel) {
		PREVIEW_CHECK(preview_select_saved(&row) == 0);
		PREVIEW_CHECK(preview_type_demo("cancel12") == 0);
		wifi_ui.input.visible = true;
		PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
		PREVIEW_CHECK(wifi_ui.display_text[0] != 0);
		event = (struct papermono_app_event) {
			.type = cancel == 0 ? PAPERMONO_APP_EVENT_BACK : PAPERMONO_APP_EVENT_HOME,
			.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON,
		};
		PREVIEW_CHECK(process_event(&event) == 0);
		PREVIEW_CHECK(preview_draft_erased());
		if (cancel != 0) {
			PREVIEW_CHECK(app_router.current == PAPERMONO_APP_HOME && !app_router.has_previous);
			PREVIEW_CHECK(find_app_descriptor(PAPERMONO_APP_WIFI)->on_close == wifi_close);
			PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_WIFI) == 0);
		}
	}

	/* All list actions are inert for each busy source, not only a visually
	 * disabled button. Assert command/state/counters together. */
	for (unsigned busy = 0; busy < 3; ++busy) {
		state = original;
		state.request_busy = busy == 0;
		state.scan_busy = busy == 1;
		if (busy == 2) state.connection = PAPERMONO_WIFI_CONNECTING;
		preview_wifi_set_state(&state);
		PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
		preview_wifi_probe_reset();
		const struct papermono_rect *actions[] = { &wifi_scan_rect, &wifi_reconnect_rect,
			&wifi_forget_rect, &wifi_previous_rect, &wifi_next_rect, &row };
		for (size_t i = 0; i < ARRAY_SIZE(actions); ++i) {
			PREVIEW_CHECK(preview_wifi_tap(actions[i], &command) == 0);
			PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_NONE);
			PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_LIST && wifi_ui.first == 0);
		}
		for (unsigned op = 0; op < PREVIEW_WIFI_OPERATIONS; ++op)
			PREVIEW_CHECK(preview_wifi_calls(op, false) == 0);
	}
	state = original;
	state.configured = false;
	state.saved_ssid[0] = 0;
	preview_wifi_set_state(&state);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_reconnect_rect, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_NONE);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_forget_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_LIST);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SAVED, false) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, false) == 0);

	preview_wifi_set_state(&original);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_forget_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_FORGET);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, false) == 0);
	PREVIEW_CHECK(preview_wifi_tap(&detail_cancel_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_LIST);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, false) == 0);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_forget_rect, &command) == 0);
	preview_wifi_result(PREVIEW_WIFI_FORGET, -EIO);
	PREVIEW_CHECK(preview_wifi_tap(&detail_confirm_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, false) == 1);
	PREVIEW_CHECK(wifi_ui.mode == WIFI_UI_LIST && wifi_ui.last_error == -EIO);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_forget_rect, &command) == 0);
	preview_wifi_result(PREVIEW_WIFI_FORGET, 0);
	PREVIEW_CHECK(preview_wifi_tap(&detail_confirm_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_FORGET, true) == 1);
	/* Positive controls keep a disabled/no-op stub from vacuously satisfying
	 * all the negative action assertions above. */
	preview_wifi_result(PREVIEW_WIFI_SCAN, -EBUSY);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_scan_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SCAN, false) == 1);
	PREVIEW_CHECK(wifi_ui.last_error == -EBUSY);
	preview_wifi_result(PREVIEW_WIFI_SAVED, -EIO);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_reconnect_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SAVED, false) == 1);
	PREVIEW_CHECK(wifi_ui.last_error == -EIO);
	preview_wifi_result(PREVIEW_WIFI_SAVED, 0);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_reconnect_rect, &command) == 0);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_SAVED, true) == 1);
	PREVIEW_CHECK(preview_wifi_calls(PREVIEW_WIFI_RESET, false) == 0);

	/* Host-testable epoch/layout guards: list paging and keyboard layer
	 * changes advance the epoch; failed display refresh cannot publish it.
	 * The physical-input queue/submit_input_event is intentionally not built. */
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	uint32_t epoch = (uint32_t)atomic_get(&wifi_input_generation);
	PREVIEW_CHECK(preview_wifi_tap(&wifi_next_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.first == 4 && atomic_get(&wifi_input_generation) != epoch);
	PREVIEW_CHECK(atomic_get(&wifi_layout_transition));
	preview_display_result(-EIO);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == -EIO);
	PREVIEW_CHECK(atomic_get(&wifi_layout_transition));
	preview_display_result(0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(!atomic_get(&wifi_layout_transition));
	PREVIEW_CHECK(preview_wifi_tap(&wifi_previous_rect, &command) == 0);
	PREVIEW_CHECK(preview_select_saved(&row) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	struct papermono_wifi_input_key key;
	PREVIEW_CHECK(papermono_wifi_input_key_at(&wifi_ui.input,
		PAPERMONO_WIFI_INPUT_CHARACTER_KEYS, &key) == 0);
	struct papermono_rect key_rect = {key.x, key.y, key.width, key.height};
	epoch = (uint32_t)atomic_get(&wifi_input_generation);
	PREVIEW_CHECK(preview_wifi_tap(&key_rect, &command) == 0);
	PREVIEW_CHECK(wifi_ui.input.layer == PAPERMONO_WIFI_INPUT_UPPER);
	PREVIEW_CHECK(atomic_get(&wifi_input_generation) != epoch);
	PREVIEW_CHECK(atomic_get(&wifi_layout_transition));
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(papermono_wifi_input_key_at(&wifi_ui.input, 0, &key) == 0);
	key_rect = (struct papermono_rect) {key.x, key.y, key.width, key.height};
	event = preview_tap(&key_rect);
	event.input_generation = epoch;
	PREVIEW_CHECK(process_event(&event) == 0 && wifi_ui.input.length == 0);
	event.input_generation = (uint32_t)atomic_get(&wifi_input_generation);
	PREVIEW_CHECK(process_event(&event) == 0 && wifi_ui.input.length == 1);
	PREVIEW_CHECK(wifi_ui.input.text[0] == key.character);

	/* Automatic sleep must also defer while the authenticated BLE sync owns
	 * an acknowledged transfer; otherwise idle timeout truncates it. */
	preview_sync_active = true;
	PREVIEW_CHECK(automatic_sleep_inhibited());
	preview_sync_active = false;
	PREVIEW_CHECK(!automatic_sleep_inhibited());

	/* RSS: index-only feed, on-demand SD body, list/detail paging and the
	 * unreadable-body error page. Bodies come from the preview SD stub. */
	struct papermono_rss_feed news_feed = {0};
	news_feed.valid = true;
	snprintf(news_feed.title, sizeof(news_feed.title), "纸面资讯");
	news_feed.count = 8U;
	for (size_t i = 0U; i < news_feed.count; ++i) {
		snprintf(news_feed.items[i].title,
			sizeof(news_feed.items[i].title),
			"第 %zu 篇文章：纸上的慢新闻", i + 1U);
		snprintf(news_feed.items[i].published,
			sizeof(news_feed.items[i].published), "2026-10-27");
		news_feed.items[i].body_length = i == 0U ? 2600U : 20U;
	}
	news_feed.updated_at = 1793000000;
	preview_rss_set_index(&news_feed);
	preview_rss_body_fail_set(false);
	preview_rss_request_result_set(0);
	const unsigned rss_requests_before_open = preview_rss_requests();
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_open + 1U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_ui.feed.count == 8U && !news_ui.detail);
	/* Drawing and system refreshes must not start more network work. A
	 * genuine exit/reopen does request an update even with a valid index. */
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_REFRESH,
		.source = PAPERMONO_APP_EVENT_SOURCE_SYSTEM,
	};
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_open + 1U);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_HOME) == 0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_open + 2U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_ui.feed.count == 8U && !news_ui.detail);
	struct papermono_rect news_row;
	PREVIEW_CHECK(papermono_interaction_grid_cell_rect(&news_list_grid, 0,
		&news_row) == 0);
	event = preview_tap(&news_row);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.detail && news_ui.selected == 0U);
	PREVIEW_CHECK(news_ui.body_loaded && news_ui.body_length > 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_REDRAW);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	/* Line-aligned pagination: the stride is an exact whole number of text
	 * lines, and the three-page stub's final line fits on its last page. */
	const int32_t news_reading_pitch =
		papermono_reading_24.line_height + NEWS_LINE_SPACE;
	const int32_t news_page_pixels = news_page_stride();
	PREVIEW_CHECK(news_page_pixels > 0);
	PREVIEW_CHECK(news_page_pixels % news_reading_pitch == 0);
	PREVIEW_CHECK(news_page_pixels <= NEWS_BODY_BOTTOM - NEWS_BODY_TOP);
	PREVIEW_CHECK(news_ui.body_pages == 3U);
	PREVIEW_CHECK(news_ui.body_height > 2 * news_page_pixels);
	PREVIEW_CHECK(news_ui.body_height <= 3 * news_page_pixels);
	PREVIEW_CHECK(strstr(news_body_text, "——正文结束") != NULL);
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_NEXT,
		.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON, .x = -1, .y = -1,
	};
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 1U);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 2U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_ui.body_height -
		(int32_t)news_ui.body_page * news_page_pixels <= news_page_pixels);
	event.type = PAPERMONO_APP_EVENT_PREVIOUS;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 1U);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 0U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	/* The page may be reading cached text when its entry-triggered fetch
	 * completes. An ordinary redraw keeps that SD body cached; an actual
	 * content update must replace both its title and visible text. */
	const unsigned rss_body_reads_before_update = preview_rss_body_loads();
	preview_rss_body_text_set("同步后的最新正文。");
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_REFRESH,
		.source = PAPERMONO_APP_EVENT_SOURCE_SYSTEM,
	};
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == rss_body_reads_before_update);
	PREVIEW_CHECK(strstr(news_body_text, "——正文结束") != NULL);
	(void)snprintf(news_feed.items[0].title,
		sizeof(news_feed.items[0].title), "已更新资讯");
	preview_rss_set_index(&news_feed);
	event.type = PAPERMONO_APP_EVENT_CONTENT_CHANGED;
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == rss_body_reads_before_update + 1U);
	PREVIEW_CHECK(strcmp(news_body_text, "同步后的最新正文。") == 0);
	PREVIEW_CHECK(strcmp(news_ui.feed.items[0].title, "已更新资讯") == 0);
	event.type = PAPERMONO_APP_EVENT_REFRESH;
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == rss_body_reads_before_update + 1U);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_open + 2U);
	preview_rss_body_text_set(NULL);
	/* Tap leaves the article; a missing SD body must render the error page,
	 * not crash or keep a stale buffer. */
	event = preview_tap(&news_body_rect);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(!news_ui.detail);
	preview_rss_body_fail_set(true);
	event = preview_tap(&news_row);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.detail && news_ui.body_length < 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	/* Reopening after the cache recovers loads the body again. */
	event = preview_tap(&news_body_rect);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	preview_rss_body_fail_set(false);
	event = preview_tap(&news_row);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_length > 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	event = preview_tap(&news_body_rect);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_LONG_PRESS,
		.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON, .x = -1, .y = -1,
	};
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_BACK);
	/* No cached index and a failing request renders the error state. */
	preview_rss_set_index(NULL);
	preview_rss_request_result_set(-ENETUNREACH);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_HOME) == 0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(news_ui.last_error == -ENETUNREACH);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	preview_rss_request_result_set(0);

	/* Pull to refresh belongs to the first list page, including its empty
	 * states. Pagination and article navigation retain their old meaning. */
	news_feed.count = 12U;
	for (size_t i = NEWS_VISIBLE_ITEMS; i < news_feed.count; ++i) {
		news_feed.items[i] = news_feed.items[0];
	}
	preview_rss_set_index(&news_feed);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_HOME) == 0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	preview_rss_status_set(0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "下拉刷新"));
	const unsigned rss_requests_before_pull = preview_rss_requests();
	enum papermono_app_event_type pull_type;
	PREVIEW_CHECK(papermono_touch_gesture_classify(240, 140, 240, 300,
		300U, 480, 800, &pull_type) == 0);
	PREVIEW_CHECK(pull_type == PAPERMONO_APP_EVENT_DOWN);
	event = (struct papermono_app_event) {
		.type = pull_type, .source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = 240, .y = 140, .duration_ms = 300U,
	};
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_REDRAW);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 1U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "正在同步"));
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "已更新资讯"));
	/* Another completed pull is still a request through the data layer's
	 * single-flight path; rendering itself never issues a request. */
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	PREVIEW_CHECK(papermono_rss_status() == 1);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	event.type = PAPERMONO_APP_EVENT_PREVIOUS;
	event.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_NONE);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	event.type = PAPERMONO_APP_EVENT_NEXT;
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.list_first == NEWS_VISIBLE_ITEMS);
	event.type = PAPERMONO_APP_EVENT_DOWN;
	event.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.list_first == 0U);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	/* Reading keeps downward swipes as previous-page, including at page 1. */
	event = preview_tap(&news_row);
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	news_ui.body_page = 1U;
	event.type = PAPERMONO_APP_EVENT_DOWN;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 0U && news_ui.detail);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(command.type == PAPERMONO_APP_COMMAND_NONE);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 2U);
	event = preview_tap(&news_body_rect);
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	preview_rss_status_set(-ETIMEDOUT);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "连接超时 · 下拉重试"));
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "已更新资讯"));
	PREVIEW_CHECK(news_ui.feed.count == 12U && !news_ui.detail);
	/* A valid-but-empty feed also exposes the failed request and recovers by
	 * the same pull instead of claiming the source is simply empty. */
	event.type = PAPERMONO_APP_EVENT_NEXT;
	event.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.list_first == NEWS_VISIBLE_ITEMS);
	news_feed.count = 0U;
	preview_rss_set_index(&news_feed);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_ui.list_first == 0U && !news_ui.detail);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "连接超时"));
	event.type = PAPERMONO_APP_EVENT_DOWN;
	event.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 3U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "正在同步资讯"));
	preview_rss_set_index(NULL);
	preview_rss_status_set(-ENETDOWN);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "网络未就绪"));
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(preview_rss_requests() == rss_requests_before_pull + 4U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "正在同步资讯"));
	preview_rss_status_set(0);

	/* Per-source tabs keep cached indexes, status and article identity apart. */
	preview_rss_defaults();
	news_feed.count = 1U;
	snprintf(news_feed.items[0].title, sizeof(news_feed.items[0].title), "金价缓存标题");
	preview_rss_set_source_index(0U, &news_feed);
	struct papermono_rss_feed ai_feed = news_feed;
	snprintf(ai_feed.items[0].title, sizeof(ai_feed.items[0].title), "AI 独立缓存标题");
	preview_rss_set_source_index(1U, &ai_feed);
	preview_rss_source_body_set(0U, "金价独立正文。");
	preview_rss_source_body_set(1U, "AI 独立正文。");
	preview_rss_source_status_set(0U, -ETIMEDOUT);
	preview_rss_source_status_set(1U, 0);
	news_sources_sync();
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	const unsigned requests_before_switch = preview_rss_requests();
	struct papermono_rect source_rect = news_source_tab_rect(1U);
	event = preview_tap(&source_rect);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.source_index == 1U && !news_ui.detail && news_ui.list_first == 0U);
	PREVIEW_CHECK(preview_rss_requests() == requests_before_switch + 1U);
	preview_rss_source_status_set(1U, 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "AI 独立缓存标题"));
	PREVIEW_CHECK(!preview_has_text(lv_screen_active(), "金价缓存标题"));
	event = preview_tap(&news_row);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(strcmp(news_body_text, "AI 独立正文。") == 0);
	const unsigned ai_body_reads = preview_rss_body_loads();
	/* A background completion for Gold cannot replace or reread AI's body. */
	preview_rss_source_body_set(0U, "金价后台更新正文。");
	preview_rss_set_source_index(0U, &news_feed);
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_CONTENT_CHANGED,
		.source = PAPERMONO_APP_EVENT_SOURCE_SYSTEM,
	};
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == ai_body_reads);
	PREVIEW_CHECK(strcmp(news_body_text, "AI 独立正文。") == 0);
	preview_rss_source_body_set(1U, "AI 新正文，同一更新时间也要刷新。");
	preview_rss_set_source_index(1U, &ai_feed);
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == ai_body_reads + 1U);
	PREVIEW_CHECK(strcmp(news_body_text, "AI 新正文，同一更新时间也要刷新。") == 0);
	struct papermono_rss_source reordered[2];
	PREVIEW_CHECK(papermono_rss_source_get(1U, &reordered[0]) == 0);
	PREVIEW_CHECK(papermono_rss_source_get(0U, &reordered[1]) == 0);
	PREVIEW_CHECK(papermono_rss_sources_replace(reordered, 2U) == 0);
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(news_ui.source_index == 0U && news_ui.detail);
	PREVIEW_CHECK(strcmp(news_ui.sources[0].id, "ai") == 0);
	PREVIEW_CHECK(preview_rss_body_loads() == ai_body_reads + 1U);
	/* Removing the current source resolves to the remaining stable id. */
	PREVIEW_CHECK(papermono_rss_sources_replace(&reordered[1], 1U) == 0);
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(!news_ui.detail && news_ui.source_index == 0U);
	PREVIEW_CHECK(strcmp(news_ui.sources[0].id, "gold") == 0);
	const unsigned requests_before_empty = preview_rss_requests();
	PREVIEW_CHECK(papermono_rss_sources_replace(NULL, 0U) == 0);
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(news_ui.source_count == 0U);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "暂无订阅"));
	PREVIEW_CHECK(preview_rss_requests() == requests_before_empty);

	/* Mixed text/tables retain every column and continue long cells by
	 * whole reading lines instead of ellipsis or clipping their tail. */
	preview_rss_defaults();
	preview_rss_set_index(&news_feed);
	preview_rss_body_text_set(preview_rss_wide_table_body);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_HOME) == 0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	event = preview_tap(&news_row);
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_doc.tables && news_ui.body_pages == 3U);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "1234.56"));
	PREVIEW_CHECK(!preview_has_text(lv_screen_active(), "1250.00"));
	news_ui.body_page = 1U;
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "1250.00"));
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "黄金"));
	news_ui.body_page = 2U;
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "测试单位"));
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "表后说明"));
	preview_make_long_table();
	preview_rss_body_text_set(preview_rss_long_table_body);
	preview_rss_set_index(&news_feed);
	event.type = PAPERMONO_APP_EVENT_CONTENT_CHANGED;
	event.source = PAPERMONO_APP_EVENT_SOURCE_SYSTEM;
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(news_doc.tables && news_ui.body_pages > 1U);
	news_ui.body_page = news_ui.body_pages - 1U;
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "长格结束"));
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "长表后的正文"));
	PREVIEW_CHECK(!news_doc.text_fallback);
	preview_make_dense_table();
	preview_rss_set_index(&news_feed);
	PREVIEW_CHECK(process_event(&event) == 0);
	news_ui.body_page = 0U;
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_doc.tables && news_ui.body_pages > 1U);
	PREVIEW_CHECK(!preview_has_text(lv_screen_active(), "行20"));
	news_ui.body_page = news_ui.body_pages - 1U;
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(preview_has_text(lv_screen_active(), "行20"));
	/* Index limits degrade to complete text, never silently lose the tail. */
	size_t overflow_used = 0U;
	for (size_t i = 0U; i < 300U; ++i) {
		overflow_used += (size_t)snprintf(preview_rss_overflow_table_body + overflow_used,
			sizeof(preview_rss_overflow_table_body) - overflow_used,
			PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_ROW
			"项目%zu\t保留数据\n" PAPERMONO_RSS_TABLE_END, i);
	}
	(void)snprintf(preview_rss_overflow_table_body + overflow_used,
		sizeof(preview_rss_overflow_table_body) - overflow_used, "结构上限后仍保留正文。");
	preview_rss_body_text_set(preview_rss_overflow_table_body);
	preview_rss_set_index(&news_feed);
	PREVIEW_CHECK(process_event(&event) == 0);
	PREVIEW_CHECK(news_doc.text_fallback && !news_doc.tables);
	PREVIEW_CHECK(strstr(news_body_text, "结构上限后仍保留正文。") != NULL);
	preview_rss_body_text_set(NULL);
	preview_rss_source_body_set(1U, NULL);
	preview_rss_defaults();

	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_HOME) == 0);
	PREVIEW_CHECK(preview_draft_erased());
	preview_wifi_probe_reset();
	preview_display_result(0);
	memset(&wifi_ui, 0, sizeof(wifi_ui));
	PREVIEW_CHECK(preview_init() == 0);
	fprintf(stderr, "preview: production Wi-Fi interaction assertions PASS "
		"(synthetic backend results; no radio/NVS worker)\n");
	return 0;
}


int preview_init(void)
{
	int ret = papermono_settings_init();
	if (ret < 0) return ret;
	(void)papermono_content_store_init();
	(void)papermono_organizer_init();
	(void)papermono_info_init();
	return papermono_app_router_init(&app_router, apps, ARRAY_SIZE(apps),
		PAPERMONO_APP_HOME);
}

int preview_render_fixture(const char *name, lv_obj_t *screen)
{
	int ret;
	preview_wifi_fixture(name);
	if (strcmp(name, "sleep") == 0) {
		memset(&wifi_ui, 0, sizeof(wifi_ui));
		lv_obj_clean(screen);
		ui_fixed_line_count = 0U;
		set_box_style(screen, paper, 0, 0);
		render_sleep_screen(screen, false);
		return 0;
	}
	clock_label = NULL;
	footer_label = NULL;
	lv_obj_clean(screen);
	ui_fixed_line_count = 0U;
	set_box_style(screen, paper, 0, 0);
	if (strncmp(name, "remote-", 7) == 0) {
		struct papermono_hid_state state = {
			.initialized = true, .connected = true, .secured = true,
			.bonded = true,
		};
		state.passkey_visible = strcmp(name, "remote-passkey") == 0;
		state.passkey = 123456U; /* Synthetic pairing code. */
		if (state.passkey_visible || strcmp(name, "remote-pairing") == 0) {
			state.secured = false;
			state.bonded = false;
		}
		preview_hid_set_state(&state);
		remote_state.mode = strcmp(name, "remote-presentation") == 0 ?
			REMOTE_MODE_PRESENTATION : strcmp(name, "remote-mouse") == 0 ?
			REMOTE_MODE_MOUSE : REMOTE_MODE_MEDIA;
		ret = papermono_app_router_open(&app_router, PAPERMONO_APP_REMOTE);
		return ret < 0 ? ret : papermono_app_router_render(&app_router, screen);
	}
	if (strncmp(name, "news", 4) == 0 || strcmp(name, "article-long") == 0) {
		struct papermono_rss_feed feed = {0};
		preview_rss_defaults();
		preview_rss_body_text_set(NULL);
		preview_rss_source_body_set(1U, NULL);
		if (strcmp(name, "news-four-sources") == 0) {
			const struct papermono_rss_source sources[] = {
				{"gold", "金价", "http://example.test/gold.xml"},
				{"ai", "AI 资讯", "http://example.test/ai.xml"},
				{"industry", "产业与技术更新", "http://example.test/industry.xml"},
				{"watch", "观察", "http://example.test/watch.xml"},
			};
			(void)papermono_rss_sources_replace(sources, 4U);
		} else if (strcmp(name, "news-no-sources") == 0) {
			(void)papermono_rss_sources_replace(NULL, 0U);
		}

		if (strcmp(name, "news-error") != 0) {
			feed.valid = true;
			snprintf(feed.title, sizeof(feed.title), "纸面资讯");
			feed.count = strcmp(name, "news-empty") == 0 ? 0U : 8U;
			for (size_t i = 0U; i < feed.count; ++i) {
				snprintf(feed.items[i].title,
					sizeof(feed.items[i].title),
					"第 %zu 篇文章：纸上的慢新闻", i + 1U);
				snprintf(feed.items[i].published,
					sizeof(feed.items[i].published),
					"2026-10-27");
				feed.items[i].body_length = i == 0U ? 2600U : 20U;
			}
			feed.updated_at = 1793000000;
		}
		preview_rss_set_index(&feed);
		if (strcmp(name, "news-ai") == 0) {
			for (size_t i = 0U; i < feed.count; ++i) {
				(void)snprintf(feed.items[i].title, sizeof(feed.items[i].title),
					"AI 资讯第 %zu 条：技术与产业观察", i + 1U);
			}
			preview_rss_set_source_index(1U, &feed);
		} else if (strcmp(name, "news-four-sources") == 0) {
			preview_rss_set_source_index(2U, &feed);
		}
		if (strcmp(name, "news-table") == 0) {
			preview_rss_body_text_set(preview_rss_table_body);
		} else if (strcmp(name, "news-table-wide") == 0 ||
			   strcmp(name, "news-table-after") == 0) {
			preview_rss_body_text_set(preview_rss_wide_table_body);
		} else if (strcmp(name, "news-table-long") == 0) {
			preview_make_long_table();
			preview_rss_body_text_set(preview_rss_long_table_body);
		} else if (strcmp(name, "news-table-dense") == 0) {
			preview_make_dense_table();
			preview_rss_body_text_set(preview_rss_long_table_body);
		}
		preview_rss_body_fail_set(false);
		preview_rss_request_result_set(strcmp(name, "news-error") == 0 ?
			-ENETUNREACH : 0);
		ret = papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS);
		if (ret < 0) return ret;
		if (strcmp(name, "news-ai") == 0 || strcmp(name, "news-four-sources") == 0) {
			const size_t source = strcmp(name, "news-ai") == 0 ? 1U : 2U;
			const struct papermono_rect rect = news_source_tab_rect(source);
			struct papermono_app_event event = preview_tap(&rect);
			struct papermono_app_command command = {0};
			ret = news_event(NULL, &event, &command);
			if (ret < 0) return ret;
			preview_rss_source_status_set(source, 0);
		}
		preview_rss_status_set(strcmp(name, "news-syncing") == 0 ? 1 :
			strcmp(name, "news-cached-error") == 0 ? -ETIMEDOUT : 0);
		if (strncmp(name, "news-table", 10U) == 0) {
			news_ui.selected = 0U;
			news_ui.detail = true;
			news_ui.body_page = 0U;
			(void)news_body_load(0U);
			news_document_prepare();
			if (strcmp(name, "news-table-wide") == 0) news_ui.body_page = 1U;
			else if (strcmp(name, "news-table-long") == 0 ||
				 strcmp(name, "news-table-after") == 0) {
				news_ui.body_page = news_ui.body_pages - 1U;
			}
		} else if (strcmp(name, "news-detail") == 0) {
			news_ui.selected = 0U;
			news_ui.detail = true;
			news_ui.body_page = 0U;
		} else if (strcmp(name, "article-long") == 0) {
			/* Last page of the three-page stub, including "——正文结束". */
			news_ui.selected = 0U;
			news_ui.detail = true;
			news_ui.body_page = 2U;
		}
		return papermono_app_router_render(&app_router, screen);
	}
	const struct {
		const char *name;
		enum papermono_app_id app;
	} pages[] = {
		{"home", PAPERMONO_APP_HOME},
		{"launcher-1", PAPERMONO_APP_LAUNCHER},
		{"launcher-2", PAPERMONO_APP_LAUNCHER},
		{"library", PAPERMONO_APP_LIBRARY},
		{"weread", PAPERMONO_APP_WEREAD},
		{"calendar", PAPERMONO_APP_CALENDAR},
		{"tasks", PAPERMONO_APP_TASKS},
		{"notes", PAPERMONO_APP_NOTES},
		{"info", PAPERMONO_APP_INFO},
		{"content-center", PAPERMONO_APP_CONTENT},
		{"remote", PAPERMONO_APP_REMOTE},
		{"tools", PAPERMONO_APP_TOOLS},
		{"system", PAPERMONO_APP_SYSTEM},
		{"system-unformatted", PAPERMONO_APP_SYSTEM},
		{"power", PAPERMONO_APP_POWER},
	};
	for (size_t i = 0; i < ARRAY_SIZE(pages); ++i) {
		if (strcmp(name, pages[i].name) != 0) continue;
		if (strcmp(name, "system-unformatted") == 0) {
			PREVIEW_CHECK(papermono_content_store_unmount() == 0);
			preview_fs_mount_result(-EINVAL);
			PREVIEW_CHECK(papermono_content_store_mount() == -EINVAL);
			PREVIEW_CHECK(papermono_content_store_media_state() == PAPERMONO_MEDIA_UNFORMATTED);
		}
		ret = papermono_app_router_open(&app_router, pages[i].app);
		if (ret < 0) return ret;
		launcher_page = strcmp(name, "launcher-2") == 0 ? 1U : 0U;
		return papermono_app_router_render(&app_router, screen);
	}
	const struct {
		const char *name;
		enum confirmation_kind kind;
	} confirmations[] = {
		{"confirm-sleep", CONFIRM_SLEEP}, {"confirm-shutdown", CONFIRM_SHUTDOWN},
		{"confirm-ble", CONFIRM_RESET_BLE}, {"confirm-reset", CONFIRM_RESET_SETTINGS},
		{"confirm-format", CONFIRM_FORMAT_TF},
	};
	for (size_t i = 0; i < ARRAY_SIZE(confirmations); ++i) {
		if (strcmp(name, confirmations[i].name) != 0) continue;
		confirmation = confirmations[i].kind;
		return render_power(NULL, screen);
	}
	if (strcmp(name, "settings") == 0) {
		ret = papermono_app_router_open(&app_router, PAPERMONO_APP_SETTINGS);
		return ret < 0 ? ret : papermono_app_router_render(&app_router, screen);
	}
	ret = papermono_app_router_open(&app_router, PAPERMONO_APP_WIFI);
	if (ret < 0) return ret;
	memset(&wifi_ui, 0, sizeof(wifi_ui));
	if (strcmp(name, "wifi-forget") == 0) {
		wifi_ui.mode = WIFI_UI_FORGET;
	} else if (strcmp(name, "wifi-list-2") == 0) {
		wifi_ui.first = 4;
	} else if (strcmp(name, "wifi-password") == 0 ||
		strcmp(name, "wifi-symbols") == 0 || strcmp(name, "wifi-63chars") == 0) {
		struct papermono_wifi_state state;
		papermono_content_server_get_wifi_state(&state);
		wifi_ui.selected = state.networks[0];
		wifi_ui.mode = WIFI_UI_PASSWORD;
		ret = papermono_wifi_input_init(&wifi_ui.input, PAPERMONO_WIFI_INPUT_PASSWORD);
		if (ret < 0) return ret;
		const char *demo = "Demo-Only! 42";
		if (strcmp(name, "wifi-63chars") == 0) {
			for (size_t i = 0; i < 63; ++i) {
				ret = papermono_wifi_input_append(&wifi_ui.input, i == 31 ? ' ' : 'W');
				if (ret < 0) return ret;
			}
			if (papermono_wifi_input_append(&wifi_ui.input, '!') != -ENOSPC)
				return -EINVAL;
			wifi_ui.input.visible = true;
			wifi_ui.last_error = -ENOSPC;
		} else {
			for (const char *c = demo; *c; ++c) {
				ret = papermono_wifi_input_append(&wifi_ui.input, *c);
				if (ret < 0) return ret;
			}
		}
		if (strcmp(name, "wifi-symbols") == 0) {
			wifi_ui.input.layer = PAPERMONO_WIFI_INPUT_SYMBOL;
			wifi_ui.input.visible = true;
		}
	}
	/* Call the actual static renderer, not an exported replacement/mock. */
	return render_wifi(NULL, screen);
}
'''


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch_ui.py <snapshot.c> <preview.c>")
    source = Path(sys.argv[1]).read_text()
    for symbol in ("static int render_wifi(", "wifi_ui", "PAPERMONO_APP_WIFI",
                   "static int render_news(", "PAPERMONO_APP_NEWS", "news_ui",
                   "news_body_text"):
        if symbol not in source:
            raise SystemExit(f"UI interface changed or incomplete: missing {symbol}")
    Path(sys.argv[2]).write_text(source + GLUE)


if __name__ == "__main__":
    main()
