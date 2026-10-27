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
void preview_rss_body_fail_set(bool fail);
unsigned preview_rss_requests(void);

#define PREVIEW_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "preview assertion failed: line %d: %s\n", \
			__LINE__, #condition); \
		return -EINVAL; \
	} \
} while (0)

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
	news_feed.count = 2U;
	snprintf(news_feed.items[0].title, sizeof(news_feed.items[0].title),
		"第一篇文章：纸上的慢新闻");
	snprintf(news_feed.items[0].published,
		sizeof(news_feed.items[0].published), "2026-10-27");
	news_feed.items[0].body_length = 4000U;
	snprintf(news_feed.items[1].title, sizeof(news_feed.items[1].title),
		"第二篇文章：设备上的阅读");
	snprintf(news_feed.items[1].published,
		sizeof(news_feed.items[1].published), "2026-10-26");
	news_feed.items[1].body_length = 14U;
	news_feed.updated_at = 1793000000;
	preview_rss_set_index(&news_feed);
	preview_rss_body_fail_set(false);
	preview_rss_request_result_set(0);
	PREVIEW_CHECK(papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS) == 0);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
	PREVIEW_CHECK(news_ui.feed.count == 2U && !news_ui.detail);
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
	PREVIEW_CHECK(news_ui.body_pages >= 2U);
	event = (struct papermono_app_event) {
		.type = PAPERMONO_APP_EVENT_NEXT,
		.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON, .x = -1, .y = -1,
	};
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 1U);
	event.type = PAPERMONO_APP_EVENT_PREVIOUS;
	command = (struct papermono_app_command) {0};
	PREVIEW_CHECK(news_event(NULL, &event, &command) == 0);
	PREVIEW_CHECK(news_ui.body_page == 0U);
	PREVIEW_CHECK(render_current(PAPERMONO_REFRESH_AUTO) == 0);
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
	if (strncmp(name, "news", 4) == 0) {
		struct papermono_rss_feed feed = {0};

		if (strcmp(name, "news-error") != 0) {
			feed.valid = true;
			snprintf(feed.title, sizeof(feed.title), "纸面资讯");
			snprintf(feed.items[0].title, sizeof(feed.items[0].title),
				"第一篇文章：纸上的慢新闻");
			snprintf(feed.items[0].published,
				sizeof(feed.items[0].published), "2026-10-27");
			feed.items[0].body_length = 4000U;
			snprintf(feed.items[1].title, sizeof(feed.items[1].title),
				"第二篇文章：设备上的阅读");
			snprintf(feed.items[1].published,
				sizeof(feed.items[1].published), "2026-10-26");
			feed.items[1].body_length = 14U;
			feed.count = strcmp(name, "news-empty") == 0 ? 0U : 2U;
			feed.updated_at = 1793000000;
		}
		preview_rss_set_index(&feed);
		preview_rss_body_fail_set(false);
		preview_rss_request_result_set(strcmp(name, "news-error") == 0 ?
			-ENETUNREACH : 0);
		ret = papermono_app_router_open(&app_router, PAPERMONO_APP_NEWS);
		if (ret < 0) return ret;
		if (strcmp(name, "news-detail") == 0) {
			news_ui.selected = 0U;
			news_ui.detail = true;
			news_ui.body_page = 0U;
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
