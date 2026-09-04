/* SPDX-License-Identifier: MIT */

/*
 * PAPERMONO MONO EDITORIAL UI CONTRACT
 * THESIS: Daily information reads like a hand-made monochrome zine; refuse the
 * repeated card dashboard while letting a strict 3 x 3 launcher grid carry a
 * different editorial composition in every cell.
 * OWN-WORLD: Pure paper and ink, oversized numerals, ruled lists, dot matrices,
 * short ink tabs, aligned rules, and one licensed ink-cat mark.
 * STORY: Read time and battery, resume a book, scan the next event and tasks, then
 * enter a function through its own visual grammar.
 * FIRST VIEWPORT: A borderless 96 px clock, open outlined reading field, ruled
 * agenda, crossed task list, and uneven bottom controls fill the 480 x 800 page.
 * FORM: User-pinned monochrome editorial collage, direct reference override,
 * seed 9baf230e.
 * FINISH: unreviewed and undocumented is unfinished; this build ends with the finish
 * review, the verdict, DESIGN.md, and every shipping raster carrying its provenance.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

#include <lvgl.h>

#include <papermono/app.h>
#include <papermono/content_store.h>
#include <papermono/content_server.h>
#include <papermono/display_scheduler.h>
#include <papermono/ft6336.h>
#include <papermono/frontlight.h>
#include <papermono/hid.h>
#include <papermono/info_provider.h>
#include <papermono/interaction.h>
#include <papermono/m5pm1.h>
#include <papermono/organizer.h>
#include <papermono/power.h>
#include <papermono/reader.h>
#include <papermono/settings.h>
#include <papermono/status_led.h>
#include <papermono/sync.h>
#include <papermono/touch_gesture.h>
#include <papermono/ui.h>
#include <papermono/weread.h>

LOG_MODULE_REGISTER(papermono_ui, LOG_LEVEL_INF);

LV_FONT_DECLARE(papermono_cjk_16);
LV_FONT_DECLARE(papermono_digits_58);
LV_FONT_DECLARE(papermono_digits_96);
LV_FONT_DECLARE(papermono_titles_28);
LV_IMAGE_DECLARE(papermono_cat_64);

#define UI_WIDTH  480
#define UI_HEIGHT 800
#define UI_HEADER_HEIGHT 72
#define UI_FOOTER_TOP 742
#define UI_TOUCH_ACTION_COOLDOWN_MS 120U
#define UI_CARD_RADIUS 0

static const lv_color_t ink = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t paper = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
static const struct device *const pmic = DEVICE_DT_GET(DT_NODELABEL(m5pm1));

struct papermono_ui_snapshot {
	struct rtc_time time;
	uint16_t battery_mv;
	uint16_t input_mv;
	uint16_t output_mv;
	uint8_t power_source;
	uint32_t uptime_minutes;
	bool rtc_valid;
	bool battery_valid;
	bool input_valid;
	bool output_valid;
	bool power_source_valid;
};

enum remote_mode {
	REMOTE_MODE_MEDIA,
	REMOTE_MODE_PRESENTATION,
	REMOTE_MODE_MOUSE,
};

struct remote_ui_state {
	enum remote_mode mode;
	int last_error;
};

static struct remote_ui_state remote_state;

struct organizer_ui_state {
	size_t calendar_first;
	size_t task_first;
	size_t note_first;
	size_t note_selected;
	size_t info_first;
	bool note_open;
};

static struct organizer_ui_state organizer_ui;

struct weread_ui_state {
	struct papermono_weread_view_state view;
	int last_error;
};

static struct weread_ui_state weread_ui;
static size_t launcher_page;

#define LIBRARY_MAX_BOOKS 32U
#define LIBRARY_VISIBLE_BOOKS 5U
#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_LARGE_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_LARGE_BSS
#endif

enum library_mode {
	LIBRARY_MODE_LIST,
	LIBRARY_MODE_READER,
	LIBRARY_MODE_MENU,
	LIBRARY_MODE_TOC,
	LIBRARY_MODE_BOOKMARKS,
};

struct library_ui_state {
	struct papermono_content_entry books[LIBRARY_MAX_BOOKS];
	struct papermono_reader_session reader;
	struct papermono_reader_page page;
	char page_text[4096];
	size_t book_count;
	size_t total_books;
	size_t selected;
	uint16_t auxiliary_selected;
	enum library_mode mode;
	int last_error;
};

static struct library_ui_state library_state PAPERMONO_LARGE_BSS;
static struct papermono_note note_view PAPERMONO_LARGE_BSS;

enum confirmation_kind {
	CONFIRM_NONE,
	CONFIRM_SLEEP,
	CONFIRM_SHUTDOWN,
	CONFIRM_RESET_WIFI,
	CONFIRM_RESET_BLE,
	CONFIRM_RESET_SETTINGS,
};

static struct papermono_app_router app_router;
static lv_obj_t *clock_label;
static lv_obj_t *footer_label;
static enum confirmation_kind confirmation;
static int power_last_error;
static int frontlight_last_error;
static int64_t last_activity_ms;
static int64_t last_refresh_ms;

static void request_redraw(struct papermono_app_command *command);

#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
static const struct device *const touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static const struct device *const buttons = DEVICE_DT_GET(DT_PATH(buttons));
static int32_t touch_x;
static int32_t touch_y;
static int32_t touch_start_x;
static int32_t touch_start_y;
static uint32_t touch_start_ms;
static uint32_t last_touch_action_ms;
static bool touch_pressed;
static atomic_t input_blocked;
#define UI_SYSTEM_EVENT_PENDING 1
#define UI_SYSTEM_EVENT_QUEUED 2
static atomic_t pending_background_refresh;
static atomic_t pending_content_refresh;
static struct k_spinlock deferred_touch_lock;
static struct papermono_app_event deferred_touch_event;
static bool deferred_touch_valid;
K_MSGQ_DEFINE(ui_events, sizeof(struct papermono_app_event), 12, 4);
#endif

#define UI_FIXED_LINE_CAPACITY 32U
static lv_point_precise_t ui_fixed_line_points[UI_FIXED_LINE_CAPACITY][2];
static size_t ui_fixed_line_count;

static void set_box_style(lv_obj_t *obj, lv_color_t background,
	uint8_t border_width, int32_t radius)
{
	lv_obj_set_style_bg_color(obj, background, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_color(obj, ink, LV_PART_MAIN);
	lv_obj_set_style_border_width(obj, border_width, LV_PART_MAIN);
	lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
	lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_ink_shape(lv_obj_t *parent, int32_t x, int32_t y,
	int32_t width, int32_t height, lv_color_t color, int32_t radius)
{
	lv_obj_t *shape = lv_obj_create(parent);
	const int32_t circle_radius = width == height && radius >= width / 2 ?
		radius : 0;

	lv_obj_set_pos(shape, x, y);
	lv_obj_set_size(shape, width, height);
	set_box_style(shape, color, 0, circle_radius);
	return shape;
}

static lv_obj_t *make_outline_shape(lv_obj_t *parent, int32_t x, int32_t y,
	int32_t width, int32_t height, uint8_t border_width, int32_t radius)
{
	lv_obj_t *shape = lv_obj_create(parent);
	const int32_t circle_radius = width == height && radius >= width / 2 ?
		radius : 0;

	lv_obj_set_pos(shape, x, y);
	lv_obj_set_size(shape, width, height);
	set_box_style(shape, paper, border_width, circle_radius);
	return shape;
}

/* Fixed point storage makes every ruled or dashed line deterministic and keeps
 * the point arrays alive for LVGL without allocating from its 32 KiB heap.
 */
static lv_obj_t *make_fixed_line(lv_obj_t *parent, int32_t x1, int32_t y1,
	int32_t x2, int32_t y2, int32_t width, int32_t dash, int32_t gap,
	lv_color_t color)
{
	const int32_t origin_x = MIN(x1, x2);
	const int32_t origin_y = MIN(y1, y2);
	lv_point_precise_t *points;
	lv_obj_t *line;

	if (ui_fixed_line_count >= ARRAY_SIZE(ui_fixed_line_points)) {
		return NULL;
	}
	points = ui_fixed_line_points[ui_fixed_line_count++];
	points[0] = (lv_point_precise_t) {
		.x = x1 - origin_x,
		.y = y1 - origin_y,
	};
	points[1] = (lv_point_precise_t) {
		.x = x2 - origin_x,
		.y = y2 - origin_y,
	};
	line = lv_line_create(parent);
	lv_line_set_points_mutable(line, points, 2U);
	lv_obj_set_pos(line, origin_x, origin_y);
	lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
	lv_obj_set_style_line_width(line, width, LV_PART_MAIN);
	lv_obj_set_style_line_rounded(line, false, LV_PART_MAIN);
	if (dash > 0 && gap > 0) {
		lv_obj_set_style_line_dash_width(line, dash, LV_PART_MAIN);
		lv_obj_set_style_line_dash_gap(line, gap, LV_PART_MAIN);
	}
	return line;
}

static void make_dot_row(lv_obj_t *parent, int32_t x, int32_t y,
	uint8_t count, uint8_t filled, int32_t size, int32_t gap)
{
	for (uint8_t index = 0U; index < count; ++index) {
		lv_obj_t *dot;

		if (index < filled) {
			dot = make_ink_shape(parent, x + index * (size + gap), y,
				size, size, ink, size / 2);
		} else {
			dot = make_outline_shape(parent,
				x + index * (size + gap), y, size, size, 2,
				size / 2);
		}
		ARG_UNUSED(dot);
	}
}

/* Launcher matrices use one crisp dashed rule per row instead of allocating a
 * separate LVGL object per mark. This keeps the dense 3 x 3 page inside the
 * 32 KiB LVGL heap while preserving the reference's rhythmic dot-grid motif.
 */
static void make_square_mark_row(lv_obj_t *parent, int32_t x, int32_t y,
	uint8_t count, int32_t size, int32_t gap, lv_color_t color)
{
	const int32_t period = size + gap;
	/* LVGL phases dashes from the absolute x coordinate.  Snap the requested
	 * origin to that phase so both ends are complete squares instead of a
	 * truncated terminal stroke on the 1-bit framebuffer.
	 */
	const int32_t aligned_x = ((x + period / 2) / period) * period;
	const int32_t end_x = aligned_x + (count - 1) * period + size;

	(void)make_fixed_line(parent, aligned_x, y + size / 2, end_x,
		y + size / 2, size, size, gap, color);
}

static void make_centered_square_mark_row(lv_obj_t *parent, int32_t x,
	int32_t width, int32_t y, uint8_t count, int32_t size, int32_t gap,
	lv_color_t color)
{
	const int32_t row_width = count * size + (count - 1) * gap;

	make_square_mark_row(parent, x + (width - row_width) / 2, y,
		count, size, gap, color);
}

static void make_checkbox(lv_obj_t *parent, int32_t x, int32_t y,
	bool checked)
{
	(void)make_outline_shape(parent, x, y, 20, 20, 2, 0);
	if (checked) {
		(void)make_fixed_line(parent, x + 4, y + 10, x + 9, y + 16,
			3, 0, 0, ink);
		(void)make_fixed_line(parent, x + 9, y + 16, x + 18, y + 3,
			3, 0, 0, ink);
	}
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
	const lv_font_t *font, lv_color_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
	return label;
}

static lv_obj_t *make_chinese_label(lv_obj_t *parent, const char *text)
{
	return make_label(parent, text, &papermono_cjk_16, ink);
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y,
	int32_t width, int32_t height)
{
	lv_obj_t *card = lv_obj_create(parent);

	lv_obj_set_pos(card, x, y);
	lv_obj_set_size(card, width, height);
	set_box_style(card, paper, 2, UI_CARD_RADIUS);
	/* A single square-ended brush rule gives legacy detail panels the shared
	 * monochrome editorial character without adding canvas buffers.
	 */
	(void)make_ink_shape(card, 16, 0, 42, 6, ink, 0);
	return card;
}

/* Abbreviated TF state for the shared header row; the full sentence is used
 * on the pages that have room for it.
 */
static const char *media_status_token(enum papermono_media_state state)
{
	switch (state) {
	case PAPERMONO_MEDIA_READY:
		return "OK";
	case PAPERMONO_MEDIA_EMPTY:
		return "EMPTY";
	case PAPERMONO_MEDIA_REMOVED:
		return "NONE";
	case PAPERMONO_MEDIA_UNFORMATTED:
		return "FS";
	case PAPERMONO_MEDIA_ERROR:
		return "ERR";
	case PAPERMONO_MEDIA_UNINITIALIZED:
	default:
		return "--";
	}
}

static void format_connectivity_status(char *status, size_t status_size)
{
	const char *wifi_status = "--";
	const char *ble_status = "--";
	const char *tf_status = media_status_token(
		papermono_content_store_media_state());

	#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	struct papermono_content_server_state network;
	papermono_content_server_get_state(&network);
	wifi_status = network.station_connected ? "LAN" :
		network.ap_enabled ? "AP" :
		network.station_connecting ? "..." : "--";
	#endif
	#if defined(CONFIG_PAPERMONO_BLE_HID)
	struct papermono_hid_state bluetooth;
	papermono_hid_get_state(&bluetooth);
	ble_status = bluetooth.connected ? "LINK" :
		bluetooth.advertising ? "ADV" :
		bluetooth.initialized ? "ON" : "--";
	#endif
	(void)snprintf(status, status_size, "W:%s  B:%s  T:%s",
		wifi_status, ble_status, tf_status);
}

static void make_header(lv_obj_t *screen, const char *title)
{
	lv_obj_t *header = lv_obj_create(screen);
	lv_obj_t *label;
	char status[48];

	format_connectivity_status(status, sizeof(status));

	lv_obj_set_pos(header, 0, 0);
	lv_obj_set_size(header, UI_WIDTH, UI_HEADER_HEIGHT);
	set_box_style(header, paper, 0, 0);
	/* Paper dominates every detail page.  A short rule carries the editorial
	 * identity without forcing a full black-to-white refresh on navigation.
	 */
	(void)make_ink_shape(header, 18, 10, 6, 34, ink, 0);
	(void)make_ink_shape(header, 18, 54, 266, 2, ink, 0);
	label = make_label(header, title, &papermono_cjk_16,
		ink);
	lv_obj_set_pos(label, 38, 17);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 244, 24);
	/* The custom CJK font is 1 bpp and also contains ASCII.  Using it for
	 * small status text avoids the broken hairlines produced when LVGL's
	 * antialiased Montserrat 14 is thresholded onto a 1-bit framebuffer.
	 */
	label = make_label(header, status, &papermono_cjk_16, ink);
	lv_obj_set_pos(label, 286, 15);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 180, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	(void)make_ink_shape(header, 286, 48, 172, 2, ink, 0);
}

static void make_footer(lv_obj_t *screen, const char *hint)
{
	lv_obj_t *line = lv_obj_create(screen);

	lv_obj_set_pos(line, 0, UI_FOOTER_TOP);
	lv_obj_set_size(line, UI_WIDTH, 2);
	set_box_style(line, ink, 0, 0);
	(void)make_ink_shape(screen, 22, 760, 42, 4, ink, 0);
	footer_label = make_chinese_label(screen, hint);
	lv_obj_set_pos(footer_label, 78, 756);
	lv_label_set_long_mode(footer_label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(footer_label, 380, 24);
}

static void format_voltage(char *buffer, size_t length, bool valid,
	uint16_t millivolts)
{
	if (!valid) {
		(void)snprintf(buffer, length, "--");
		return;
	}
	(void)snprintf(buffer, length, "%u.%03u V", millivolts / 1000U,
		millivolts % 1000U);
}

static bool rtc_time_is_valid(const struct rtc_time *time)
{
	const int year = time->tm_year + 1900;

	return year >= 2020 && year <= 2099 && time->tm_mon >= 0 &&
		time->tm_mon < 12 && time->tm_mday >= 1 && time->tm_mday <= 31 &&
		time->tm_hour >= 0 && time->tm_hour < 24 && time->tm_min >= 0 &&
		time->tm_min < 60;
}

static void read_snapshot(struct papermono_ui_snapshot *snapshot)
{
	int ret;

	*snapshot = (struct papermono_ui_snapshot) {
		.uptime_minutes = (uint32_t)(k_uptime_get() / 60000),
	};
	if (device_is_ready(rtc)) {
		ret = rtc_get_time(rtc, &snapshot->time);
		snapshot->rtc_valid = ret == 0 && rtc_time_is_valid(&snapshot->time);
	}
	if (!device_is_ready(pmic)) {
		return;
	}
	ret = m5pm1_read_voltage(pmic, M5PM1_VOLTAGE_BATTERY,
		&snapshot->battery_mv);
	snapshot->battery_valid = ret == 0;
	ret = m5pm1_read_voltage(pmic, M5PM1_VOLTAGE_INPUT,
		&snapshot->input_mv);
	snapshot->input_valid = ret == 0;
	ret = m5pm1_read_voltage(pmic, M5PM1_VOLTAGE_5V_OUTPUT,
		&snapshot->output_mv);
	snapshot->output_valid = ret == 0;
	ret = m5pm1_get_power_source(pmic, &snapshot->power_source);
	snapshot->power_source_valid = ret == 0;
}

static const char *reader_density_name(enum papermono_reader_density density)
{
	return density == PAPERMONO_READER_DENSITY_COMPACT ? "紧凑" :
		density == PAPERMONO_READER_DENSITY_RELAXED ? "宽松" : "标准";
}

static void reader_visual_layout(enum papermono_reader_density density,
	int32_t *margin, int32_t *line_space)
{
	if (density == PAPERMONO_READER_DENSITY_COMPACT) {
		*margin = 16;
		*line_space = 2;
	} else if (density == PAPERMONO_READER_DENSITY_RELAXED) {
		*margin = 32;
		*line_space = 8;
	} else {
		*margin = 22;
		*line_space = 4;
	}
}

static void add_card_text_tone(lv_obj_t *card, const char *title,
	const char *body, const char *meta, lv_color_t color)
{
	int32_t width;
	int32_t height;
	int32_t body_y;
	int32_t body_height;
	bool compact_meta;
	lv_obj_t *label;

	/* LVGL resolves object coordinates lazily. Reading a newly-created card's
	 * width or height before a layout pass returns zero, which collapses the
	 * body label and places metadata above the card. The title happened not to
	 * depend on those values, making the cards look half-rendered.
	 */
	lv_obj_update_layout(card);
	width = MAX(lv_obj_get_width(card) - 32, 1);
	height = lv_obj_get_height(card);
	compact_meta = meta != NULL && height <= 120;

	label = make_label(card, title, &papermono_cjk_16, color);
	lv_obj_set_pos(label, 16, 12);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, width, 22);
	if (compact_meta) {
		lv_obj_set_width(label, MAX(width / 2 - 8, 1));
	}
	if (height <= 72) {
		const int32_t title_width = MAX((width * 2) / 5, 1);

		lv_obj_set_size(label, title_width, 22);
		label = make_label(card, body, &papermono_cjk_16, color);
		lv_obj_set_pos(label, 24 + title_width, 12);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, MAX(width - title_width - 8, 1), 22);
		return;
	}
	label = make_label(card, body, &papermono_cjk_16, color);
	body_y = compact_meta ? 44 : 47;
	body_height = compact_meta ? height - body_y - 8 :
		meta != NULL ? height - 32 - body_y : height - body_y - 10;
	lv_obj_set_pos(label, 16, body_y);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_size(label, width, MAX(body_height, 1));
	if (meta != NULL) {
		label = make_label(card, meta, &papermono_cjk_16, color);
		lv_obj_set_pos(label, compact_meta ? 16 + width / 2 : 16,
			compact_meta ? 12 : height - 28);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, compact_meta ? MAX(width / 2, 1) : width,
			18);
	}
}

static void add_card_text(lv_obj_t *card, const char *title,
	const char *body, const char *meta)
{
	add_card_text_tone(card, title, body, meta, ink);
}

enum home_region_index {
	HOME_REGION_READER,
	HOME_REGION_CALENDAR,
	HOME_REGION_TASKS,
	HOME_REGION_LAUNCHER,
	HOME_REGION_SYSTEM,
	HOME_REGION_POWER,
};

static const struct papermono_interaction_region home_regions[] = {
	[HOME_REGION_READER] = { { 18, 194, 444, 176 },
		PAPERMONO_APP_LIBRARY, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_CALENDAR] = { { 18, 394, 444, 112 },
		PAPERMONO_APP_CALENDAR, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_TASKS] = { { 18, 522, 444, 104 },
		PAPERMONO_APP_TASKS, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_LAUNCHER] = { { 18, 644, 252, 88 },
		PAPERMONO_APP_LAUNCHER, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_SYSTEM] = { { 278, 644, 88, 88 },
		PAPERMONO_APP_SYSTEM, 0, PAPERMONO_INTERACTION_TAP },
	[HOME_REGION_POWER] = { { 374, 644, 88, 88 },
		PAPERMONO_APP_POWER, 0, PAPERMONO_INTERACTION_TAP },
};

enum launcher_motif {
	LAUNCHER_MOTIF_LIBRARY,
	LAUNCHER_MOTIF_WEREAD,
	LAUNCHER_MOTIF_CALENDAR,
	LAUNCHER_MOTIF_TASKS,
	LAUNCHER_MOTIF_NOTES,
	LAUNCHER_MOTIF_REMOTE,
	LAUNCHER_MOTIF_CONTENT,
	LAUNCHER_MOTIF_INFO,
	LAUNCHER_MOTIF_TOOLS,
	LAUNCHER_MOTIF_SETTINGS,
	LAUNCHER_MOTIF_POWER,
};

struct launcher_layout_item {
	enum papermono_app_id id;
	enum launcher_motif motif;
};

#define LAUNCHER_PAGE_SIZE 9U

static const struct papermono_interaction_grid_layout launcher_grid = {
	.x = 18,
	.y = 112,
	.cell_width = 148,
	.cell_height = 210,
	.column_gap = 0,
	.row_gap = 0,
	.columns = 3,
	.rows = 3,
};

static const struct launcher_layout_item launcher_items[] = {
	{ PAPERMONO_APP_LIBRARY, LAUNCHER_MOTIF_LIBRARY },
	{ PAPERMONO_APP_WEREAD, LAUNCHER_MOTIF_WEREAD },
	{ PAPERMONO_APP_CALENDAR, LAUNCHER_MOTIF_CALENDAR },
	{ PAPERMONO_APP_TASKS, LAUNCHER_MOTIF_TASKS },
	{ PAPERMONO_APP_NOTES, LAUNCHER_MOTIF_NOTES },
	{ PAPERMONO_APP_REMOTE, LAUNCHER_MOTIF_REMOTE },
	{ PAPERMONO_APP_CONTENT, LAUNCHER_MOTIF_CONTENT },
	{ PAPERMONO_APP_INFO, LAUNCHER_MOTIF_INFO },
	{ PAPERMONO_APP_TOOLS, LAUNCHER_MOTIF_TOOLS },
	{ PAPERMONO_APP_SETTINGS, LAUNCHER_MOTIF_SETTINGS },
	{ PAPERMONO_APP_POWER, LAUNCHER_MOTIF_POWER },
};

static const struct papermono_app_descriptor *find_app_descriptor(
	enum papermono_app_id id)
{
	for (size_t index = 0U; index < app_router.app_count; ++index) {
		if (app_router.apps[index].id == id) {
			return &app_router.apps[index];
		}
	}
	return NULL;
}

static int render_home(void *context, lv_obj_t *screen)
{
	struct papermono_ui_snapshot snapshot;
	struct papermono_content_entry recent_book;
	struct papermono_calendar_event next_event;
	struct papermono_task active_task = { 0 };
	struct papermono_task completed_task = { 0 };
	const struct papermono_rect *reader =
		&home_regions[HOME_REGION_READER].rect;
	const struct papermono_rect *calendar =
		&home_regions[HOME_REGION_CALENDAR].rect;
	const struct papermono_rect *tasks =
		&home_regions[HOME_REGION_TASKS].rect;
	lv_obj_t *label;
	lv_obj_t *button;
	lv_obj_t *brand_mark;
	char status[48];
	char battery[16];
	char event_time[16];
	char event_date[24];
	bool have_active_task = false;
	bool have_completed_task = false;
	const int recent_ret = papermono_reader_recent(&recent_book);
	uint8_t reader_progress = 0U;
	bool reader_progress_valid = false;

	ARG_UNUSED(context);
	read_snapshot(&snapshot);
	format_connectivity_status(status, sizeof(status));
	format_voltage(battery, sizeof(battery), snapshot.battery_valid,
		snapshot.battery_mv);

	/* Reference-led masthead: a real icon, one wordmark and a hairline. */
	brand_mark = lv_image_create(screen);
	lv_image_set_src(brand_mark, &papermono_cat_64);
	lv_obj_set_pos(brand_mark, 12, -4);
	label = make_label(screen, "PAPER MONO", &lv_font_montserrat_18, ink);
	lv_obj_set_pos(label, 74, 12);
	lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
	label = make_label(screen, status, &papermono_cjk_16, ink);
	lv_obj_set_pos(label, 286, 13);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 176, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	/* Keep the masthead rule clear of the cat silhouette. */
	(void)make_fixed_line(screen, 74, 46, 462, 46, 2, 0, 0, ink);

	clock_label = snapshot.rtc_valid ?
		make_label(screen, "00:00", &papermono_digits_96, ink) :
		make_label(screen, "TIME", &lv_font_montserrat_48, ink);
	lv_obj_set_pos(clock_label, snapshot.rtc_valid ? 14 : 20,
		snapshot.rtc_valid ? 45 : 72);
	lv_obj_set_size(clock_label, 360, snapshot.rtc_valid ? 92 : 56);
	label = make_chinese_label(screen, "时钟尚未校准");
	lv_obj_set_pos(label, 20, snapshot.rtc_valid ? 150 : 134);
	if (snapshot.rtc_valid) {
		const struct papermono_preferences *prefs = papermono_settings_get();
		int hour = snapshot.time.tm_hour;
		const char *period = NULL;

		if (!prefs->use_24_hour) {
			period = hour >= 12 ? "PM" : "AM";
			hour %= 12;
			hour = hour == 0 ? 12 : hour;
		}
		lv_label_set_text_fmt(clock_label, "%02d:%02d", hour,
			snapshot.time.tm_min);
		lv_label_set_text_fmt(label, "%04d / %02d / %02d  %s",
			snapshot.time.tm_year + 1900, snapshot.time.tm_mon + 1,
			snapshot.time.tm_mday, period == NULL ? "24H" : period);
	}
	label = make_label(screen, battery, &lv_font_montserrat_18, ink);
	lv_obj_set_pos(label, 374, 62);
	lv_obj_set_size(label, 82, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	label = make_label(screen, "BATTERY", &lv_font_montserrat_14, ink);
	lv_obj_set_pos(label, 388, 88);
	(void)make_fixed_line(screen, 386, 112, 456, 112, 2, 0, 0, ink);

	if (recent_ret == 0 && library_state.reader.page_loaded &&
	    strcmp(library_state.reader.book.id, recent_book.id) == 0) {
		reader_progress = library_state.page.approximate_percent;
		reader_progress_valid = true;
	}
	(void)make_outline_shape(screen, reader->x, reader->y,
		reader->width, reader->height, 2, 0);
	(void)make_ink_shape(screen, reader->x + 310, reader->y,
		2, reader->height, ink, 0);
	button = make_outline_shape(screen, reader->x + 18, reader->y + 16,
		72, 28, 2, 0);
	label = make_label(button, "READ", &lv_font_montserrat_14, ink);
	lv_obj_center(label);
	label = make_label(screen, "最近阅读", &papermono_titles_28, ink);
	lv_obj_set_pos(label, reader->x + 104, reader->y + 12);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 184, 34);
	label = make_label(screen, recent_ret == 0 ? recent_book.title :
		"还没有最近阅读", &papermono_cjk_16, ink);
	lv_obj_set_pos(label, reader->x + 18, reader->y + 62);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 274, 44);
	if (recent_ret == 0) {
		label = make_label(screen,
			recent_book.kind == PAPERMONO_CONTENT_EPUB ?
			"EPUB · 最近阅读" : "TXT · 最近阅读",
			&papermono_cjk_16, ink);
		lv_obj_set_pos(label, reader->x + 18, reader->y + 116);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 154, 22);
	}
	button = make_outline_shape(screen, reader->x + 188, reader->y + 132,
		104, 34, 2, 0);
	label = make_label(button, "打开书库", &papermono_cjk_16, ink);
	lv_obj_center(label);

	label = make_chinese_label(screen, "进度");
	lv_obj_set_pos(label, 332, reader->y + 8);
	lv_obj_set_size(label, 128, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	if (reader_progress_valid) {
		label = make_label(screen, "00", &papermono_digits_58, ink);
		lv_obj_set_pos(label, 340, reader->y + 30);
		lv_label_set_text_fmt(label, "%02u", reader_progress);
		label = make_label(screen, "%", &lv_font_montserrat_28, ink);
		lv_obj_set_pos(label, 424, reader->y + 60);
		make_dot_row(screen, 346, reader->y + 128, 7U,
			(uint8_t)CLAMP((reader_progress + 14U) / 15U, 0U, 7U),
			10, 6);
	} else {
		label = make_chinese_label(screen,
			recent_ret == 0 ? "已有记录" : "未开始");
		lv_obj_set_pos(label, 332, reader->y + 64);
		lv_obj_set_size(label, 128, 22);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
			LV_PART_MAIN);
	}

	const struct papermono_calendar_time now = {
		.year = snapshot.time.tm_year + 1900,
		.month = snapshot.time.tm_mon + 1,
		.day = snapshot.time.tm_mday,
		.hour = snapshot.time.tm_hour,
		.minute = snapshot.time.tm_min,
		.second = snapshot.time.tm_sec,
	};
	const bool have_event = snapshot.rtc_valid &&
		papermono_organizer_get_next_event(&now, &next_event) == 0;
	label = make_label(screen, "下一日程", &papermono_titles_28, ink);
	lv_obj_set_pos(label, calendar->x, calendar->y);
	label = make_label(screen, "NEXT", &lv_font_montserrat_14, ink);
	lv_obj_set_pos(label, calendar->x + 144, calendar->y + 8);
	(void)make_ink_shape(screen, calendar->x, calendar->y + 40,
		calendar->width, 2, ink, 0);
	if (have_event) {
		(void)snprintf(event_date, sizeof(event_date), "%04d/%02u/%02u",
			next_event.start.year, next_event.start.month,
			next_event.start.day);
		label = make_label(screen, event_date, &lv_font_montserrat_14, ink);
		lv_obj_set_pos(label, calendar->x + 328, calendar->y + 8);
		(void)snprintf(event_time, sizeof(event_time), "%02u:%02u",
			next_event.start.hour, next_event.start.minute);
		button = make_outline_shape(screen, calendar->x + 4,
			calendar->y + 56, 78, 48, 2, 0);
		label = make_label(button, event_time, &lv_font_montserrat_18, ink);
		lv_obj_center(label);
		label = make_label(screen, next_event.title, &papermono_cjk_16, ink);
		lv_obj_set_pos(label, calendar->x + 104, calendar->y + 60);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 324, 22);
	} else {
		label = make_chinese_label(screen, snapshot.rtc_valid ?
			"暂无日程" : "时间未校准");
		lv_obj_set_pos(label, calendar->x + 4, calendar->y + 60);
		label = make_chinese_label(screen, snapshot.rtc_valid ?
			"从内容中心同步" : "校时后显示下一日程");
		lv_obj_set_pos(label, calendar->x + 104, calendar->y + 60);
	}

	for (size_t index = 0U; index < papermono_organizer_task_count(); ++index) {
		struct papermono_task candidate;

		if (papermono_organizer_get_task(index, &candidate) < 0) {
			continue;
		}
		if (!candidate.done && !have_active_task) {
			active_task = candidate;
			have_active_task = true;
		} else if (candidate.done && !have_completed_task) {
			completed_task = candidate;
			have_completed_task = true;
		}
		if (have_active_task && have_completed_task) {
			break;
		}
	}
	label = make_label(screen, "待办", &papermono_titles_28, ink);
	lv_obj_set_pos(label, tasks->x, tasks->y);
	label = make_label(screen, "TO DO", &lv_font_montserrat_14, ink);
	lv_obj_set_pos(label, tasks->x + 144, tasks->y + 8);
	(void)make_ink_shape(screen, tasks->x, tasks->y + 40,
		tasks->width, 2, ink, 0);
	if (have_active_task) {
		make_checkbox(screen, tasks->x + 4, tasks->y + 46, false);
	}
	label = make_label(screen, have_active_task ? active_task.title :
		have_completed_task ? "所有待办已完成" : "没有未完成待办",
		&papermono_cjk_16, ink);
	lv_obj_set_pos(label, tasks->x + (have_active_task ? 36 : 4),
		tasks->y + 46);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 392, 22);
	(void)make_ink_shape(screen, tasks->x + (have_active_task ? 36 : 4),
		tasks->y + 72, tasks->width - (have_active_task ? 36 : 4),
		1, ink, 0);
	if (have_completed_task) {
		make_checkbox(screen, tasks->x + 4, tasks->y + 78, true);
	}
	label = make_label(screen, have_completed_task ? completed_task.title :
		"轻触查看全部待办", &papermono_cjk_16, ink);
	lv_obj_set_pos(label, tasks->x + (have_completed_task ? 36 : 4),
		tasks->y + 78);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label,
		tasks->width - (have_completed_task ? 36 : 4), 22);
	if (have_completed_task) {
		(void)make_fixed_line(screen, tasks->x + 36, tasks->y + 90,
			tasks->x + 280, tasks->y + 90, 4, 0, 0, ink);
	}

	const struct papermono_rect *launcher =
		&home_regions[HOME_REGION_LAUNCHER].rect;
	(void)make_outline_shape(screen, launcher->x, launcher->y,
		launcher->width, launcher->height, 2, 0);
	label = make_label(screen, "全部应用", &papermono_titles_28, ink);
	lv_obj_set_pos(label, launcher->x + 16, launcher->y + 12);
	label = make_label(screen, "00", &papermono_digits_58, ink);
	lv_obj_set_pos(label, launcher->x + 176, launcher->y + 8);
	lv_label_set_text_fmt(label, "%02u",
		(unsigned int)ARRAY_SIZE(launcher_items));
	button = make_outline_shape(screen, launcher->x + 16, launcher->y + 56,
		104, 24, 2, 0);
	label = make_label(button, "OPEN", &lv_font_montserrat_14, ink);
	lv_obj_center(label);

	const struct papermono_rect *system =
		&home_regions[HOME_REGION_SYSTEM].rect;
	(void)make_outline_shape(screen, system->x, system->y,
		system->width, system->height, 2, 0);
	label = make_chinese_label(screen, "系统");
	lv_obj_set_pos(label, system->x + 28, system->y + 12);
	(void)make_fixed_line(screen, system->x + 22, system->y + 48,
		system->x + 66, system->y + 48, 2, 0, 0, ink);
	(void)make_fixed_line(screen, system->x + 22, system->y + 60,
		system->x + 54, system->y + 60, 4, 0, 0, ink);
	(void)make_fixed_line(screen, system->x + 22, system->y + 74,
		system->x + 78, system->y + 74, 2, 0, 0, ink);

	const struct papermono_rect *power =
		&home_regions[HOME_REGION_POWER].rect;
	(void)make_outline_shape(screen, power->x, power->y,
		power->width, power->height, 2, 0);
	label = make_label(screen, "OFF", &lv_font_montserrat_28, ink);
	lv_obj_set_pos(label, power->x, power->y + 12);
	lv_obj_set_size(label, power->width, 34);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	label = make_chinese_label(screen, "休眠");
	lv_obj_set_pos(label, power->x, power->y + 52);
	lv_obj_set_size(label, power->width, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

	(void)make_fixed_line(screen, 18, 752, 462, 752, 1, 0, 0, ink);
	footer_label = make_chinese_label(screen, "轻触打开 · 边缘右滑返回");
	lv_obj_set_pos(footer_label, 18, 768);
	lv_obj_set_size(footer_label, 444, 22);
	return 0;
}

static void render_launcher_item(lv_obj_t *screen,
	const struct launcher_layout_item *item,
	const struct papermono_rect *rect,
	const struct papermono_ui_snapshot *snapshot)
{
	const struct papermono_app_descriptor *app =
		find_app_descriptor(item->id);
	lv_obj_t *label;
	lv_obj_t *button;

	if (app == NULL) {
		return;
	}
	/* Every cell shares one title rail and one content baseline.  The motif may
	 * vary below it, but its structure never drifts from the grid.
	 */
	label = make_label(screen, app->title, &papermono_titles_28, ink);
	lv_obj_set_pos(label, rect->x + 12, rect->y + 12);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 124, 34);
	(void)make_ink_shape(screen, rect->x + 12, rect->y + 52,
		124, 2, ink, 0);
	switch (item->motif) {
	case LAUNCHER_MOTIF_LIBRARY: {
		struct papermono_content_entry recent;
		const int recent_ret = papermono_reader_recent(&recent);

		label = make_label(screen, "READ", &lv_font_montserrat_18, ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 68);
		for (uint8_t index = 0U; index < 3U; ++index) {
			lv_obj_t *spine = make_outline_shape(screen,
				rect->x + 72 + index * 20, rect->y + 68,
				16, 64, 2, 0);

			(void)make_ink_shape(spine, 3, 8, 10, 2, ink, 0);
			(void)make_ink_shape(spine, 3, 52, 10, 2, ink, 0);
		}
		(void)make_ink_shape(screen, rect->x + 12, rect->y + 144,
			120, 2, ink, 0);
		label = make_label(screen, recent_ret == 0 ? recent.title :
			"尚无最近阅读", &papermono_cjk_16, ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 172);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 124, 24);
		break;
	}
	case LAUNCHER_MOTIF_WEREAD:
		(void)make_outline_shape(screen, rect->x + 12, rect->y + 68,
			124, 48, 2, 0);
		label = make_label(screen, "LOCAL", &lv_font_montserrat_18, ink);
		lv_obj_set_pos(label, rect->x + 40, rect->y + 82);
		label = make_chinese_label(screen, "本地书架");
		lv_obj_set_pos(label, rect->x + 34, rect->y + 128);
		make_centered_square_mark_row(screen, rect->x,
			rect->width, rect->y + 172, 7U, 8, 6, ink);
		break;
	case LAUNCHER_MOTIF_CALENDAR: {
		char day[4] = "--";

		if (snapshot->rtc_valid) {
			(void)snprintf(day, sizeof(day), "%02d", snapshot->time.tm_mday);
		}
		label = make_label(screen, day, &papermono_digits_58, ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 64);
		label = make_label(screen, "DAY", &lv_font_montserrat_14, ink);
		lv_obj_set_pos(label, rect->x + 96, rect->y + 96);
		make_square_mark_row(screen, rect->x + 16, rect->y + 144, 7U,
			8, 6, ink);
		make_centered_square_mark_row(screen, rect->x, rect->width,
			rect->y + 172, snapshot->rtc_valid ? 6U : 3U,
			8, snapshot->rtc_valid ? 6 : 4, ink);
		break;
	}
	case LAUNCHER_MOTIF_TASKS:
		make_checkbox(screen, rect->x + 12, rect->y + 68, false);
		(void)make_fixed_line(screen, rect->x + 42, rect->y + 78,
			rect->x + 132, rect->y + 78, 2, 0, 0, ink);
		make_checkbox(screen, rect->x + 12, rect->y + 104, false);
		(void)make_fixed_line(screen, rect->x + 42, rect->y + 114,
			rect->x + 128, rect->y + 114, 2, 0, 0, ink);
		make_checkbox(screen, rect->x + 12, rect->y + 144, false);
		(void)make_fixed_line(screen, rect->x + 42, rect->y + 154,
			rect->x + 118, rect->y + 154, 2, 0, 0, ink);
		break;
	case LAUNCHER_MOTIF_NOTES:
		(void)make_ink_shape(screen, rect->x + 12, rect->y + 68,
			4, 104, ink, 0);
		for (uint8_t index = 0U; index < 5U; ++index) {
			(void)make_fixed_line(screen, rect->x + 32,
				rect->y + 76 + index * 24, rect->x + 132,
				rect->y + 76 + index * 24, index == 2U ? 4 : 2,
				0, 0, ink);
		}
		break;
	case LAUNCHER_MOTIF_REMOTE:
		button = make_outline_shape(screen, rect->x + 40, rect->y + 68,
			68, 24, 2, 0);
		label = make_label(button, "HID", &lv_font_montserrat_14, ink);
		lv_obj_center(label);
		for (uint8_t index = 0U; index < 3U; ++index) {
			lv_obj_t *key = make_outline_shape(screen,
				rect->x + 10 + index * 44, rect->y + 104,
				38, 44, index == 1U ? 4 : 2, 0);
			label = make_label(key, index == 0U ? "<" :
				index == 1U ? "II" : ">", &lv_font_montserrat_18,
				ink);
			lv_obj_center(label);
		}
		label = make_label(screen, "BLE HID", &lv_font_montserrat_18, ink);
		lv_obj_set_pos(label, rect->x, rect->y + 172);
		lv_obj_set_size(label, rect->width, 22);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
			LV_PART_MAIN);
		break;
	case LAUNCHER_MOTIF_CONTENT:
		label = make_label(screen, "SYNC", &lv_font_montserrat_28, ink);
		lv_obj_set_pos(label, rect->x, rect->y + 68);
		lv_obj_set_size(label, rect->width, 34);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
			LV_PART_MAIN);
		label = make_label(screen, "LOCAL / WIFI", &lv_font_montserrat_14, ink);
		lv_obj_set_pos(label, rect->x, rect->y + 118);
		lv_obj_set_size(label, rect->width, 20);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
			LV_PART_MAIN);
		button = make_outline_shape(screen, rect->x + 26, rect->y + 160,
			96, 24, 2, 0);
		label = make_label(button, "UPLOAD", &lv_font_montserrat_14, ink);
		lv_obj_center(label);
		break;
	case LAUNCHER_MOTIF_INFO: {
		const size_t info_count = papermono_info_count();

		label = make_label(screen, "00", &papermono_digits_58, ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 64);
		lv_label_set_text_fmt(label, "%02u", (unsigned int)MIN(info_count,
			(size_t)99U));
		label = make_label(screen, "CARDS", &lv_font_montserrat_14, ink);
		lv_obj_set_pos(label, rect->x + 96, rect->y + 96);
		for (uint8_t index = 0U; index < 3U; ++index) {
			(void)make_ink_shape(screen, rect->x + 12,
				rect->y + 144 + index * 16,
				120 - index * 20, 2, ink, 0);
		}
		break;
	}
	case LAUNCHER_MOTIF_TOOLS:
		make_square_mark_row(screen, rect->x + 24, rect->y + 72, 6U,
			10, 8, ink);
		make_square_mark_row(screen, rect->x + 32, rect->y + 100, 5U,
			10, 8, ink);
		make_square_mark_row(screen, rect->x + 42, rect->y + 128, 4U,
			10, 8, ink);
		label = make_label(screen, "I2C / SPI", &lv_font_montserrat_14, ink);
		lv_obj_set_pos(label, rect->x + 46, rect->y + 172);
		break;
	case LAUNCHER_MOTIF_SETTINGS:
		for (uint8_t index = 0U; index < 3U; ++index) {
			const int32_t y = rect->y + 76 + index * 40;

			(void)make_fixed_line(screen, rect->x + 14, y,
				rect->x + 132, y, 2, 0, 0, ink);
			(void)make_ink_shape(screen,
				rect->x + 38 + index * 32, y - 5,
				10, 10, ink, 5);
		}
		break;
	case LAUNCHER_MOTIF_POWER:
		label = make_label(screen, "OFF", &lv_font_montserrat_48, ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 64);
		label = make_label(screen, "SLEEP / POWER", &lv_font_montserrat_14,
			ink);
		lv_obj_set_pos(label, rect->x + 12, rect->y + 160);
		(void)make_ink_shape(screen, rect->x + 12, rect->y + 186,
			108, 4, ink, 0);
		break;
	}
}

static int render_launcher(void *context, lv_obj_t *screen)
{
	struct papermono_ui_snapshot snapshot;
	struct papermono_rect rect;
	lv_obj_t *label;
	char status[48];
	char page_text[12];
	const size_t page_count =
		(ARRAY_SIZE(launcher_items) + LAUNCHER_PAGE_SIZE - 1U) /
		LAUNCHER_PAGE_SIZE;
	const size_t first_item = launcher_page * LAUNCHER_PAGE_SIZE;

	ARG_UNUSED(context);
	read_snapshot(&snapshot);
	format_connectivity_status(status, sizeof(status));
	label = make_label(screen, "PAPER MONO / APP INDEX",
		&lv_font_montserrat_14, ink);
	lv_obj_set_pos(label, 18, 14);
	lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
	label = make_label(screen, status, &papermono_cjk_16, ink);
	lv_obj_set_pos(label, 238, 12);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 224, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	label = make_label(screen, "应用", &papermono_titles_28, ink);
	lv_obj_set_pos(label, 18, 50);
	label = make_label(screen, "00", &lv_font_montserrat_28, ink);
	lv_obj_set_pos(label, 366, 48);
	lv_obj_set_size(label, 96, 34);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	lv_label_set_text_fmt(label, "%02u",
		(unsigned int)ARRAY_SIZE(launcher_items));
	(void)make_ink_shape(screen, 18, 96, 444, 2, ink, 0);

	(void)snprintf(page_text, sizeof(page_text), "%02u / %02u",
		(unsigned int)(launcher_page + 1U), (unsigned int)page_count);
	label = make_label(screen, page_text, &lv_font_montserrat_14, ink);
	lv_obj_set_pos(label, 366, 76);
	lv_obj_set_size(label, 96, 20);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	(void)make_outline_shape(screen, launcher_grid.x, launcher_grid.y,
		launcher_grid.cell_width * launcher_grid.columns,
		launcher_grid.cell_height * launcher_grid.rows, 2, 0);
	for (size_t slot = 0U; slot < LAUNCHER_PAGE_SIZE; ++slot) {
		const size_t index = first_item + slot;

		if (index >= ARRAY_SIZE(launcher_items) ||
		    papermono_interaction_grid_cell_rect(&launcher_grid, slot,
			&rect) < 0) {
			continue;
		}
		render_launcher_item(screen, &launcher_items[index], &rect,
			&snapshot);
	}
	/* Shared rules are drawn last so every composition lands on the same common
	 * borders. The result is one true nine-grid, not nine floating cards.
	 */
	(void)make_fixed_line(screen, 166, 112, 166, 742, 2, 0, 0, ink);
	(void)make_fixed_line(screen, 314, 112, 314, 742, 2, 0, 0, ink);
	(void)make_fixed_line(screen, 18, 322, 462, 322, 2, 0, 0, ink);
	(void)make_fixed_line(screen, 18, 532, 462, 532, 2, 0, 0, ink);
	(void)make_fixed_line(screen, 18, 760, 462, 760, 1, 0, 0, ink);
	footer_label = make_chinese_label(screen,
		"轻触进入 · 左右键换页 · 边缘右滑返回");
	lv_obj_set_pos(footer_label, 18, 772);
	lv_obj_set_size(footer_label, 444, 22);
	return 0;
}

static int render_content_center(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	make_header(screen, "内容中心");
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	struct papermono_content_server_state state;
	char body[192];
	lv_obj_t *card;

	papermono_content_server_get_state(&state);
	(void)snprintf(body, sizeof(body), "网络：%s\n密码：%s\n状态：%s",
		state.ap_enabled ? state.ap_ssid : "接入点未启用",
		state.ap_enabled ? state.ap_password : "--",
		state.ap_verified ? "接口自检通过" :
		state.ap_enabled ? "正在验证" : "未启用");
	card = make_card(screen, 22, 96, 436, 154);
	add_card_text(card, "首次配网接入点", body,
		state.ap_enabled ? "192.168.4.1" : "SOFTAP OFF");

	(void)snprintf(body, sizeof(body),
		state.client_paired ? "本地会话已建立\n浏览器刷新会自动恢复" :
		"浏览器打开设备地址\n输入一次性配对码：%s",
		state.pairing_code);
	card = make_card(screen, 22, 270, 436, 154);
	add_card_text(card, "浏览器会话", body,
		state.client_paired ? "SESSION READY" : state.pairing_code);

	const char *address = state.station_ipv4[0] == '\0' ? "等待 DHCP 地址" :
		state.station_ipv4;
	(void)snprintf(body, sizeof(body), "局域网：%s\n内容版本：%u",
		state.station_connected ? address :
		state.station_connecting ? "正在连接" : "未连接",
		state.content_generation);
	card = make_card(screen, 22, 444, 436, 154);
	add_card_text(card, "连接状态", body,
		state.station_connected ? "LAN READY" :
		state.ap_enabled ? "SETUP MODE" : "OFFLINE");

	card = make_card(screen, 22, 618, 436, 96);
	add_card_text(card, "管理范围", "书籍 · 日历 · 待办 · 便签", NULL);
#else
	lv_obj_t *card = make_card(screen, 22, 116, 436, 230);
	add_card_text(card, "内容中心未启用", "当前构建未包含本地 HTTP 服务。",
		"CONFIG DISABLED");
#endif
	make_footer(screen, "浏览器传输在后台执行  ·  左键返回");
	return 0;
}

static const char *weread_kind_text(enum papermono_weread_kind kind)
{
	switch (kind) {
	case PAPERMONO_WEREAD_AUDIO:
		return "有声书";
	case PAPERMONO_WEREAD_ARTICLES:
		return "公众号文章";
	case PAPERMONO_WEREAD_EBOOK:
	default:
		return "电子书";
	}
}

static const char *weread_error_text(int error)
{
	switch (error) {
	case 0:
		return "书架为空";
	case -ENOENT:
		return "尚未导入 weread-v1.json";
	case -ENODEV:
		return "TF 卡未插入";
	case -EFBIG:
		return "书架文件超过 64 KiB";
	case -EILSEQ:
		return "书架文件不是有效 UTF-8";
	case -EBADMSG:
	case -EPROTONOSUPPORT:
		return "书架文件格式或版本不受支持";
	default:
		return "书架读取失败";
	}
}

static int weread_open(void *context)
{
	struct weread_ui_state *state = context;

	if (state == NULL) {
		return -EINVAL;
	}
	papermono_weread_view_reset(&state->view);
	state->last_error = papermono_weread_reload();
	return 0;
}

static const struct papermono_interaction_grid_layout weread_book_grid = {
	.x = 22,
	.y = 132,
	.cell_width = 436,
	.cell_height = 104,
	.column_gap = 0,
	.row_gap = 12,
	.columns = 1,
	.rows = PAPERMONO_WEREAD_VISIBLE_BOOKS,
};

static int render_weread_empty(struct weread_ui_state *state,
	lv_obj_t *screen)
{
	lv_obj_t *card;
	char body[192];

	make_header(screen, "微信读书 · 实验");
	card = make_card(screen, 22, 104, 436, 166);
	add_card_text(card, "本地书架不可用",
		weread_error_text(state->last_error), "LOCAL ONLY");
	card = make_card(screen, 22, 294, 436, 190);
	add_card_text(card, "从内容中心导入",
		"上传固定文件名 weread-v1.json\n内容类型选择“微信读书书架”",
		"点按打开内容中心");
	card = make_card(screen, 22, 508, 436, 184);
	(void)snprintf(body, sizeof(body),
		"路径：/papermono/weread/%s\n不保存账号、Cookie 或书籍正文",
		PAPERMONO_WEREAD_CATALOG_NAME);
	add_card_text(card, "当前安全边界", body, "CREDENTIAL FREE");
	make_footer(screen, "长按重载 · 边缘右滑或点按底部返回");
	return 0;
}

static int render_weread_list(struct weread_ui_state *state,
	lv_obj_t *screen, const struct papermono_weread_status *status)
{
	char summary[160];
	lv_obj_t *label;
	const size_t first = papermono_weread_view_first(&state->view);

	make_header(screen, "微信读书 · 本地书架");
	(void)snprintf(summary, sizeof(summary), "%s · %zu/%zu 本%s",
		status->source[0] == '\0' ? "LOCAL" : status->source,
		status->count, status->total, status->truncated ? " · 已截断" : "");
	label = make_chinese_label(screen, summary);
	lv_obj_set_pos(label, 24, 78);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 432, 20);
	(void)snprintf(summary, sizeof(summary), "同步时间：%s",
		status->synced_at[0] == '\0' ? "--" : status->synced_at);
	label = make_chinese_label(screen, summary);
	lv_obj_set_pos(label, 24, 102);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, 432, 20);

	for (size_t row = 0U; row < PAPERMONO_WEREAD_VISIBLE_BOOKS; ++row) {
		const size_t index = first + row;
		struct papermono_weread_book book;
		char meta[160];
		char progress[48];
		lv_obj_t *card;

		if (index >= status->count || papermono_weread_get(index, &book) < 0) {
			break;
		}
		card = make_card(screen, 22, 132 + (int32_t)row * 116, 436, 104);
		if (index == state->view.selected) {
			lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
		}
		label = make_chinese_label(card, book.title);
		lv_obj_set_pos(label, 14, 10);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 292, 22);
		(void)snprintf(meta, sizeof(meta), "%s · %s",
			book.author[0] == '\0' ? "作者未知" : book.author,
			weread_kind_text(book.kind));
		label = make_chinese_label(card, meta);
		lv_obj_set_pos(label, 14, 42);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 310, 20);
		(void)snprintf(meta, sizeof(meta), "%s%s",
			book.updated_at[0] == '\0' ? "未提供更新时间" : book.updated_at,
			book.finished ? " · 已读完" : "");
		label = make_chinese_label(card, meta);
		lv_obj_set_pos(label, 14, 72);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, 300, 20);
		if (book.has_progress) {
			(void)snprintf(progress, sizeof(progress), "%u%%", book.progress);
		} else {
			(void)snprintf(progress, sizeof(progress), "--");
		}
		label = make_label(card, progress, &lv_font_montserrat_28, ink);
		lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -16, 18);
	}
	make_footer(screen, "点按查看详情 · 左右键选择 · 边缘右滑返回");
	return 0;
}

static int render_weread_detail(struct weread_ui_state *state,
	lv_obj_t *screen)
{
	struct papermono_weread_book book;
	char body[320];
	char duration[64];
	lv_obj_t *card;

	if (papermono_weread_get(state->view.selected, &book) < 0) {
		state->view.mode = PAPERMONO_WEREAD_VIEW_LIST;
		state->last_error = -ENOENT;
		return render_weread_empty(state, screen);
	}
	make_header(screen, "微信读书 · 书籍详情");
	const uint32_t minutes = book.reading_seconds / 60U;
	if (minutes >= 60U) {
		(void)snprintf(duration, sizeof(duration), "%u 小时 %u 分钟",
			minutes / 60U, minutes % 60U);
	} else {
		(void)snprintf(duration, sizeof(duration), "%u 分钟", minutes);
	}
	char progress[32];

	if (book.has_progress) {
		(void)snprintf(progress, sizeof(progress), "%u%%", book.progress);
	} else {
		(void)snprintf(progress, sizeof(progress), "未提供");
	}
	(void)snprintf(body, sizeof(body),
		"作者：%s\n类型：%s\n阅读进度：%s\n累计阅读：%s\n更新时间：%s",
		book.author[0] == '\0' ? "未知" : book.author,
		weread_kind_text(book.kind), progress, duration,
		book.updated_at[0] == '\0' ? "--" : book.updated_at);
	card = make_card(screen, 22, 104, 436, 292);
	add_card_text(card, book.title, body, book.finished ? "已读完" : book.id);
	card = make_card(screen, 22, 424, 436, 232);
	add_card_text(card, "第一阶段说明",
		"当前只显示电脑或手机同步的书架摘要。\n不连接私有接口，不下载微信读书正文。",
		"后续直连能力将独立评审");
	make_footer(screen, "边缘右滑返回书架 · 左右键切换书籍");
	return 0;
}

static int render_weread(void *context, lv_obj_t *screen)
{
	struct weread_ui_state *state = context;
	struct papermono_weread_status status;

	if (state == NULL) {
		return -EINVAL;
	}
	papermono_weread_get_status(&status);
	state->last_error = status.last_error;
	if (status.last_error < 0 || status.count == 0U) {
		return render_weread_empty(state, screen);
	}
	if (state->view.mode == PAPERMONO_WEREAD_VIEW_DETAIL) {
		return render_weread_detail(state, screen);
	}
	return render_weread_list(state, screen, &status);
}

static int library_scan_book(const struct papermono_content_entry *entry,
	void *context)
{
	struct library_ui_state *state = context;

	++state->total_books;
	if (state->book_count < ARRAY_SIZE(state->books)) {
		state->books[state->book_count++] = *entry;
	}
	return 0;
}

static void library_sort(struct library_ui_state *state)
{
	for (size_t index = 1U; index < state->book_count; ++index) {
		struct papermono_content_entry value = state->books[index];
		size_t target = index;

		while (target > 0U &&
		       strcmp(state->books[target - 1U].title, value.title) > 0) {
			state->books[target] = state->books[target - 1U];
			--target;
		}
		state->books[target] = value;
	}
}

static int library_open(void *context)
{
	struct library_ui_state *state = context;
	const struct papermono_preferences *prefs = papermono_settings_get();
	const bool resume_reader = prefs->active_app == PAPERMONO_APP_LIBRARY &&
		prefs->reader_resume;

	if (!resume_reader && prefs->reader_resume &&
	    papermono_settings_set_reader_resume(false) < 0) {
		LOG_WRN("Unable to clear stale reader resume state");
	}

	state->book_count = 0U;
	state->total_books = 0U;
	state->selected = 0U;
	state->mode = LIBRARY_MODE_LIST;
	state->last_error = papermono_content_store_scan_books(
		library_scan_book, state, NULL);
	if (state->last_error == 0) {
		library_sort(state);
		struct papermono_content_entry recent;

		if (papermono_reader_recent(&recent) == 0) {
			bool recent_found = false;

			for (size_t index = 0U; index < state->book_count; ++index) {
				if (strcmp(state->books[index].id, recent.id) == 0) {
					state->selected = index;
					recent_found = true;
					break;
				}
			}
			/* A persisted Library app means the device slept or restarted while
			 * reading. Resume the saved reader position; an ordinary navigation
			 * into Library still opens the list because the previous app remains
			 * the persisted active app until this on_open() call completes.
			 */
			if (resume_reader && recent_found) {
				state->last_error = papermono_reader_open(&state->reader,
					&state->books[state->selected]);
				if (state->last_error == 0) {
					state->mode = LIBRARY_MODE_READER;
				} else {
					(void)papermono_settings_set_reader_resume(false);
				}
			}
		}
	}
	if (resume_reader && state->mode != LIBRARY_MODE_READER) {
		(void)papermono_settings_set_reader_resume(false);
	}
	return 0;
}

static void library_close(void *context)
{
	struct library_ui_state *state = context;

	if (state->mode != LIBRARY_MODE_LIST) {
		(void)papermono_reader_save(&state->reader);
	} else {
		(void)papermono_settings_set_reader_resume(false);
	}
}

static void library_return_to_list(struct library_ui_state *state)
{
	(void)papermono_reader_save(&state->reader);
	(void)papermono_settings_set_reader_resume(false);
	state->mode = LIBRARY_MODE_LIST;
}

static int render_library_list(struct library_ui_state *state,
	lv_obj_t *screen)
{
	lv_obj_t *label;
	char summary[80];

	make_header(screen, "书库");
	if (state->book_count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 260);
		const enum papermono_media_state media =
			papermono_content_store_media_state();

		add_card_text(card, "没有可显示的书籍",
			papermono_content_store_state_text(media),
			"/SD/PAPERMONO/BOOKS");
		card = make_card(screen, 22, 424, 436, 170);
		add_card_text(card, "支持格式",
			"UTF-8 / GBK TXT，以及无 DRM 的文字型 EPUB。单本上限 64 MiB。",
			"TXT · EPUB");
		make_footer(screen, "长按首卡重新扫描  ·  向右滑动返回");
		return 0;
	}

	const size_t first = (state->selected / LIBRARY_VISIBLE_BOOKS) *
		LIBRARY_VISIBLE_BOOKS;
	if (state->last_error < 0) {
		(void)snprintf(summary, sizeof(summary),
			"打开或扫描失败（%d） · 仍显示 %u 本书", state->last_error,
			(unsigned int)state->book_count);
	} else {
		(void)snprintf(summary, sizeof(summary), "%u 本书%s",
			(unsigned int)state->total_books,
			state->total_books > state->book_count ?
				"（仅显示前 32 本）" : "");
	}
	label = make_chinese_label(screen, summary);
	lv_obj_set_pos(label, 24, 84);
	for (size_t row = 0U; row < LIBRARY_VISIBLE_BOOKS; ++row) {
		const size_t index = first + row;
		lv_obj_t *card;
		char meta[48];

		if (index >= state->book_count) {
			break;
		}
		card = make_card(screen, 22, 116 + (int32_t)row * 120, 436, 102);
		if (index == state->selected) {
			lv_obj_set_style_border_width(card, 5, LV_PART_MAIN);
		}
		(void)snprintf(meta, sizeof(meta), "%s · %llu KiB",
			state->books[index].kind == PAPERMONO_CONTENT_TXT ?
				"TXT" : "EPUB",
			(unsigned long long)((state->books[index].size + 1023U) /
				1024U));
		add_card_text(card, state->books[index].title, "轻触打开", meta);
	}
	make_footer(screen, state->last_error < 0 ?
		"文件可能损坏或已移除  ·  长按重新扫描" :
		"左右键选择  ·  轻触打开  ·  长按重新扫描");
	return 0;
}

static int render_library_reader(struct library_ui_state *state,
	lv_obj_t *screen)
{
	const enum papermono_reader_density density =
		papermono_settings_get()->reader_density;
	lv_obj_t *label;
	char footer[128];
	int32_t margin;
	int32_t line_space;

	reader_visual_layout(density, &margin, &line_space);

	make_header(screen, state->reader.book.kind == PAPERMONO_CONTENT_EPUB ?
		state->reader.epub.chapters[state->reader.chapter].title :
		state->reader.book.title);
	state->last_error = papermono_reader_read_page(&state->reader,
		state->page_text, sizeof(state->page_text), &state->page);
	if (state->last_error < 0) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 250);
		char error[48];

		(void)snprintf(error, sizeof(error), "读取失败（%d）",
			state->last_error);
		add_card_text(card, "无法读取书籍", error,
			papermono_content_store_state_text(
				papermono_content_store_media_state()));
	} else {
		label = make_chinese_label(screen, state->page_text);
		lv_obj_set_pos(label, margin, 88);
		lv_obj_set_size(label, UI_WIDTH - margin * 2, 636);
		lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_line_space(label, line_space, LV_PART_MAIN);
	}
	/* Only the current page number is shown: the total would require paging
	 * through the whole book, which is far too costly on an e-paper device.
	 * Density is omitted here because the reading menu already shows it.
	 */
	(void)snprintf(footer, sizeof(footer),
		"第 %u 页 · %u%% · %s · %s · 左右键翻页",
		(unsigned int)(state->reader.page_number + 1U),
		state->page.approximate_percent,
		papermono_text_encoding_name(state->reader.encoding),
		papermono_reader_is_bookmarked(&state->reader) ?
			"已加书签" : "未加书签");
	make_footer(screen, footer);
	return 0;
}

/* The frontlight hangs off the display device and is reset to off whenever
 * the panel re-initialises, including after a deep-sleep wake. The saved
 * level is therefore re-applied on every UI start as well as on change.
 */
static const struct device *frontlight_display;

static int apply_frontlight(void)
{
	const struct papermono_preferences *prefs = papermono_settings_get();
	uint8_t level;
	int ret;

	if (frontlight_display == NULL) {
		return -ENODEV;
	}
	/* The driver applies its own perceptual curve on top of this value. */
	ret = papermono_frontlight_output_level(prefs->frontlight_percent, &level);
	return ret < 0 ? ret : display_set_brightness(frontlight_display, level);
}

static int set_frontlight(uint8_t percent)
{
	const uint8_t previous = papermono_settings_get()->frontlight_percent;
	uint8_t output;
	int ret;

	ret = papermono_frontlight_output_level(percent, &output);
	if (ret < 0 || frontlight_display == NULL) {
		frontlight_last_error = ret < 0 ? ret : -ENODEV;
		return frontlight_last_error;
	}
	/* Apply first so an unavailable PWM value is never persisted. If NVS then
	 * fails, restore the old physical level so hardware and settings agree.
	 */
	ret = display_set_brightness(frontlight_display, output);
	if (ret == 0) {
		ret = papermono_settings_set_frontlight(percent);
		if (ret < 0) {
			uint8_t restore;

			if (papermono_frontlight_output_level(previous, &restore) == 0) {
				const int restore_ret = display_set_brightness(
					frontlight_display, restore);

				if (restore_ret < 0) {
					LOG_ERR("Unable to restore frontlight after settings failure (%d)",
						restore_ret);
				}
			}
		}
	}
	frontlight_last_error = ret;
	if (ret < 0) {
		LOG_WRN("Unable to set frontlight to %u%% (%d)", percent, ret);
	}
	return ret;
}

static int render_library_menu(struct library_ui_state *state,
	lv_obj_t *screen)
{
	const struct papermono_preferences *prefs = papermono_settings_get();
	lv_obj_t *card;
	char body[96];

	make_header(screen, "阅读菜单");
	card = make_card(screen, 22, 86, 436, 84);
	add_card_text(card, papermono_reader_is_bookmarked(&state->reader) ?
		"移除当前页书签" : "添加当前页书签",
		"保存当前书籍位置", NULL);
	card = make_card(screen, 22, 178, 436, 84);
	add_card_text(card, "目录",
		state->reader.book.kind == PAPERMONO_CONTENT_EPUB ?
			"浏览并跳转章节" : "TXT 没有内置目录", NULL);
	(void)snprintf(body, sizeof(body), "密度：%s；编码：%s",
		reader_density_name(papermono_settings_get()->reader_density),
		papermono_text_encoding_name(state->reader.encoding));
	card = make_card(screen, 22, 270, 436, 84);
	add_card_text(card, "排版与编码", body,
		state->reader.book.kind == PAPERMONO_CONTENT_TXT ?
			"轻触密度 · 长按编码" : "轻触切换密度");
	(void)snprintf(body, sizeof(body), prefs->frontlight_percent == 0U ?
		"已关闭" : "%u%%", prefs->frontlight_percent);
	card = make_card(screen, 22, 362, 436, 84);
	add_card_text(card, "前光亮度", body, frontlight_last_error < 0 ?
		"调整失败 · 请重试" : "轻触/右滑增加 · 左滑减少 · 长按关闭");
	card = make_card(screen, 22, 454, 436, 84);
	(void)snprintf(body, sizeof(body), "已保存 %u / %u 个位置",
		state->reader.bookmark_count, PAPERMONO_READER_BOOKMARK_LIMIT);
	add_card_text(card, "书签列表", body, NULL);
	card = make_card(screen, 22, 546, 436, 84);
	add_card_text(card, "返回阅读", "关闭此菜单", NULL);
	card = make_card(screen, 22, 638, 436, 84);
	add_card_text(card, "返回书库", "保留当前阅读进度", NULL);
	make_footer(screen, "阅读进度和书签保存在内部 NVS");
	return 0;
}

static int render_library_toc(struct library_ui_state *state,
	lv_obj_t *screen)
{
	make_header(screen, "目录");
	const uint16_t first = (state->auxiliary_selected / 5U) * 5U;
	const uint16_t toc_count = state->reader.epub.toc_count > 0U ?
		state->reader.epub.toc_count : state->reader.epub.chapter_count;

	for (uint16_t row = 0U; row < 5U; ++row) {
		const uint16_t toc_index = first + row;

		if (toc_index >= toc_count) {
			break;
		}
		const struct papermono_epub_toc_entry *toc =
			state->reader.epub.toc_count > 0U ?
			&state->reader.epub.toc[toc_index] : NULL;
		const uint16_t chapter = toc == NULL ? toc_index : toc->chapter;
		const uint8_t level = toc == NULL ? 0U : toc->level;
		const int32_t indent = MIN(level, 4U) * 16;
		lv_obj_t *card;
		char meta[48];

		if (chapter >= state->reader.epub.chapter_count) {
			break;
		}
		card = make_card(screen, 22 + indent, 96 + (int32_t)row * 124,
			436 - indent, 106);
		if (toc_index == state->auxiliary_selected) {
			lv_obj_set_style_border_width(card, 5, LV_PART_MAIN);
		}
		(void)snprintf(meta, sizeof(meta), "层级 %u · 章节 %u / %u",
			level + 1U, chapter + 1U, state->reader.epub.chapter_count);
		add_card_text(card, toc == NULL ?
			state->reader.epub.chapters[chapter].title : toc->title,
			chapter == state->reader.chapter ? "当前章节" : "轻触跳转",
			meta);
	}
	make_footer(screen, "左右键选择  ·  轻触跳转  ·  左边界返回");
	return 0;
}

static int render_library_bookmarks(struct library_ui_state *state,
	lv_obj_t *screen)
{
	make_header(screen, "书签");
	if (state->reader.bookmark_count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 220);

		add_card_text(card, "尚无书签",
			"阅读时长按页面，或在阅读菜单中添加当前页书签。", NULL);
		make_footer(screen, "左键返回阅读菜单");
		return 0;
	}
	const uint16_t first = (state->auxiliary_selected / 5U) * 5U;

	for (uint16_t row = 0U; row < 5U; ++row) {
		const uint16_t index = first + row;
		const struct papermono_reader_bookmark *mark;
		lv_obj_t *card;
		char title[48];
		char meta[48];

		if (index >= state->reader.bookmark_count) {
			break;
		}
		mark = &state->reader.bookmarks[index];
		card = make_card(screen, 22, 96 + (int32_t)row * 124, 436, 106);
		if (index == state->auxiliary_selected) {
			lv_obj_set_style_border_width(card, 5, LV_PART_MAIN);
		}
		/* Lead with the stored snippet; fall back to the ordinal for
		 * bookmarks saved before previews existed.
		 */
		if (mark->preview[0] != '\0') {
			(void)snprintf(title, sizeof(title), "%s", mark->preview);
		} else {
			(void)snprintf(title, sizeof(title), "书签 %u",
				index + 1U);
		}
		if (state->reader.book.kind == PAPERMONO_CONTENT_EPUB &&
		    mark->chapter < state->reader.epub.chapter_count) {
			(void)snprintf(meta, sizeof(meta), "%s · 位置 %u",
				state->reader.epub.chapters[mark->chapter].title,
				mark->offset);
		} else {
			(void)snprintf(meta, sizeof(meta), "文件位置 %u", mark->offset);
		}
		add_card_text(card, title, "轻触跳转", meta);
	}
	make_footer(screen, "左右键选择  ·  轻触跳转  ·  左边界返回");
	return 0;
}

static int render_library(void *context, lv_obj_t *screen)
{
	struct library_ui_state *state = context;

	if (state->mode == LIBRARY_MODE_READER) {
		return render_library_reader(state, screen);
	}
	if (state->mode == LIBRARY_MODE_MENU) {
		return render_library_menu(state, screen);
	}
	if (state->mode == LIBRARY_MODE_TOC) {
		return render_library_toc(state, screen);
	}
	if (state->mode == LIBRARY_MODE_BOOKMARKS) {
		return render_library_bookmarks(state, screen);
	}
	return render_library_list(state, screen);
}

static const struct papermono_interaction_grid_layout remote_tab_grid = {
	.x = 22,
	.y = 212,
	.cell_width = 140,
	.cell_height = 64,
	.column_gap = 8,
	.row_gap = 0,
	.columns = 3,
	.rows = 1,
};

static const struct papermono_interaction_grid_layout remote_action_grid = {
	.x = 22,
	.y = 296,
	.cell_width = 214,
	.cell_height = 112,
	.column_gap = 8,
	.row_gap = 18,
	.columns = 2,
	.rows = 3,
};

static const struct papermono_rect remote_mouse_pad = {
	22, 296, 436, 372,
};

static void remote_action_card(lv_obj_t *screen, int column, int row,
	const char *title, const char *meta)
{
	struct papermono_rect rect;

	if (papermono_interaction_grid_cell_rect(&remote_action_grid,
		(size_t)row * remote_action_grid.columns + (size_t)column,
		&rect) < 0) {
		return;
	}
	lv_obj_t *card = make_card(screen, rect.x, rect.y, rect.width,
		rect.height);

	add_card_text(card, title, "轻触发送", meta);
}

static void remote_status_card(lv_obj_t *screen, int column, int row,
	const char *title, const char *body, const char *meta)
{
	struct papermono_rect rect;

	if (papermono_interaction_grid_cell_rect(&remote_action_grid,
		(size_t)row * remote_action_grid.columns + (size_t)column,
		&rect) < 0) {
		return;
	}
	lv_obj_t *card = make_card(screen, rect.x, rect.y, rect.width,
		rect.height);

	add_card_text(card, title, body, meta);
}

static int render_remote(void *context, lv_obj_t *screen)
{
	struct remote_ui_state *remote = context;
	struct papermono_ui_snapshot snapshot;
	lv_obj_t *card;
	lv_obj_t *label;
	char body[128];
	struct papermono_hid_state state = { 0 };

	make_header(screen, "电脑遥控器");
	read_snapshot(&snapshot);
#if defined(CONFIG_PAPERMONO_BLE_HID)
	papermono_hid_get_state(&state);
#endif
	if (state.passkey_visible) {
		(void)snprintf(body, sizeof(body),
			"在电脑上输入配对码\n%06u", state.passkey);
	} else if (state.connected) {
		(void)snprintf(body, sizeof(body), "已连接%s%s%s%s",
			state.secured ? " · 安全连接" : " · 正在完成配对",
			state.bonded ? " · 已保存主机" : "",
			state.suspended ? " · 主机已挂起" : "",
			remote->last_error < 0 ? " · 操作失败" : "");
	} else if (state.advertising) {
		(void)snprintf(body, sizeof(body),
			"正在等待电脑连接\n蓝牙名称：PaperMono C153");
	} else {
		(void)snprintf(body, sizeof(body), "蓝牙 HID 未就绪 · 错误 %d",
			state.last_error);
	}
	if (snapshot.battery_valid) {
		const uint8_t percent = (uint8_t)CLAMP(
			((int)snapshot.battery_mv - 3300) * 100 / 900, 0, 100);
		const size_t used = strlen(body);
		(void)snprintf(body + used, sizeof(body) - used, "\n电池：%u%%",
			percent);
#if defined(CONFIG_PAPERMONO_BLE_HID)
		if (state.initialized) {
			(void)papermono_hid_set_battery_level(percent);
		}
#endif
	}
	card = make_card(screen, 22, 84, 436, 120);
	add_card_text(card, state.passkey_visible ? "蓝牙配对" : "连接状态",
		body, state.connected ? "CONNECTED" : "HID OVER GATT");

	static const char *const tabs[] = { "媒体", "演示", "鼠标" };
	for (int index = 0; index < 3; ++index) {
		struct papermono_rect rect;

		if (papermono_interaction_grid_cell_rect(&remote_tab_grid,
			(size_t)index, &rect) < 0) {
			continue;
		}
		card = make_card(screen, rect.x, rect.y, rect.width, rect.height);
		if (remote->mode == (enum remote_mode)index) {
			lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
			(void)make_ink_shape(card, 18, rect.height - 10,
				rect.width - 36, 4, ink, 0);
		}
		label = make_chinese_label(card, tabs[index]);
		lv_obj_center(label);
	}

	if (remote->mode == REMOTE_MODE_MEDIA) {
		remote_action_card(screen, 0, 0, "播放 / 暂停", "PLAY");
		remote_action_card(screen, 1, 0, "静音", "MUTE");
		remote_action_card(screen, 0, 1, "上一首", "PREVIOUS");
		remote_action_card(screen, 1, 1, "下一首", "NEXT");
		remote_action_card(screen, 0, 2, "减小音量", "VOLUME -");
		remote_action_card(screen, 1, 2, "增大音量", "VOLUME +");
	} else if (remote->mode == REMOTE_MODE_PRESENTATION) {
		remote_action_card(screen, 0, 0, "上一页", "PAGE UP");
		remote_action_card(screen, 1, 0, "下一页", "PAGE DOWN");
		remote_action_card(screen, 0, 1, "开始放映", "F5");
		remote_action_card(screen, 1, 1, "退出放映", "ESC");
		remote_action_card(screen, 0, 2, "黑屏切换", "B");
		remote_status_card(screen, 1, 2, "连接状态",
			state.connected ? "已连接，可以发送指令" : "请先完成电脑配对",
			state.connected ? "READY" : "PAIR FIRST");
	} else {
		card = make_card(screen, 22, 296, 436, 372);
		add_card_text(card, "触摸板",
			"轻触：左键\n长按：右键\n中部滑动：移动指针\n右侧上下滑动：滚轮",
			"RELATIVE MOUSE");
		card = make_card(screen, 22, 684, 436, 42);
		add_card_text(card, "实体键", "左键 / 右键点击", NULL);
	}
	make_footer(screen, "顶部切换模式  ·  轻触页脚返回");
	return 0;
}

static int remote_send(struct remote_ui_state *remote,
	enum papermono_hid_action_type type, int8_t x, int8_t y, int8_t wheel,
	uint8_t buttons)
{
#if defined(CONFIG_PAPERMONO_BLE_HID)
	const struct papermono_hid_action action = {
		.type = type,
		.mouse_x = x,
		.mouse_y = y,
		.wheel = wheel,
		.buttons = buttons,
	};

	remote->last_error = papermono_hid_send_action(&action);
	return remote->last_error;
#else
	ARG_UNUSED(type);
	ARG_UNUSED(x);
	ARG_UNUSED(y);
	ARG_UNUSED(wheel);
	ARG_UNUSED(buttons);
	remote->last_error = -ENOTSUP;
	return -ENOTSUP;
#endif
}

static int remote_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct remote_ui_state *remote = context;
	size_t grid_index;
	int ret = 0;

	if (event->type == PAPERMONO_APP_EVENT_BACK ||
	    (event->type == PAPERMONO_APP_EVENT_TAP &&
	     event->y >= UI_FOOTER_TOP)) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
		return 0;
	}
	if (papermono_interaction_spaced_grid_index(event,
		PAPERMONO_INTERACTION_TAP, &remote_tab_grid, &grid_index) == 0) {
		remote->mode = (enum remote_mode)grid_index;
		request_redraw(command);
		return 0;
	}

	if (remote->mode == REMOTE_MODE_MEDIA) {
		if (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_MEDIA_PREVIOUS,
				0, 0, 0, 0);
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT &&
			   papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_MEDIA_NEXT,
				0, 0, 0, 0);
		} else if (papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_TAP, &remote_action_grid,
			&grid_index) == 0) {
			static const enum papermono_hid_action_type actions[3][2] = {
				{ PAPERMONO_HID_MEDIA_PLAY_PAUSE, PAPERMONO_HID_MEDIA_MUTE },
				{ PAPERMONO_HID_MEDIA_PREVIOUS, PAPERMONO_HID_MEDIA_NEXT },
				{ PAPERMONO_HID_MEDIA_VOLUME_DOWN, PAPERMONO_HID_MEDIA_VOLUME_UP },
			};
			ret = remote_send(remote,
				actions[grid_index / 2U][grid_index % 2U], 0, 0, 0, 0);
		}
	} else if (remote->mode == REMOTE_MODE_PRESENTATION) {
		if (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_PRESENT_PREVIOUS,
				0, 0, 0, 0);
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT &&
			   papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_PRESENT_NEXT,
				0, 0, 0, 0);
		} else if (papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_TAP, &remote_action_grid,
			&grid_index) == 0) {
			static const enum papermono_hid_action_type actions[3][2] = {
				{ PAPERMONO_HID_PRESENT_PREVIOUS, PAPERMONO_HID_PRESENT_NEXT },
				{ PAPERMONO_HID_PRESENT_START, PAPERMONO_HID_PRESENT_EXIT },
				{ PAPERMONO_HID_PRESENT_BLACK, PAPERMONO_HID_PRESENT_BLACK },
			};
			if (grid_index != 5U) {
				ret = remote_send(remote,
					actions[grid_index / 2U][grid_index % 2U],
					0, 0, 0, 0);
			}
		}
	} else {
		if (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0, 0, 0, BIT(0));
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT &&
			   papermono_app_event_is_button(event)) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0, 0, 0, BIT(1));
		} else if (papermono_interaction_contains(event,
			PAPERMONO_INTERACTION_TAP, &remote_mouse_pad)) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0, 0, 0, BIT(0));
		} else if (papermono_interaction_contains(event,
			PAPERMONO_INTERACTION_LONG_PRESS, &remote_mouse_pad)) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0, 0, 0, BIT(1));
		} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
			   papermono_app_event_is_touch(event) &&
			   event->x >= remote_mouse_pad.x &&
			   event->x < remote_mouse_pad.x + remote_mouse_pad.width &&
			   event->y >= remote_mouse_pad.y &&
			   event->y < remote_mouse_pad.y + remote_mouse_pad.height) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 40, 0, 0, 0);
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT &&
			   papermono_app_event_is_touch(event) &&
			   event->x >= remote_mouse_pad.x &&
			   event->x < remote_mouse_pad.x + remote_mouse_pad.width &&
			   event->y >= remote_mouse_pad.y &&
			   event->y < remote_mouse_pad.y + remote_mouse_pad.height) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, -40, 0, 0, 0);
		} else if (event->type == PAPERMONO_APP_EVENT_UP &&
			   papermono_app_event_is_touch(event) &&
			   event->x >= remote_mouse_pad.x &&
			   event->x < remote_mouse_pad.x + remote_mouse_pad.width &&
			   event->y >= remote_mouse_pad.y &&
			   event->y < remote_mouse_pad.y + remote_mouse_pad.height) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0,
				event->x >= 360 ? 0 : -40,
				event->x >= 360 ? 1 : 0, 0);
		} else if (event->type == PAPERMONO_APP_EVENT_DOWN &&
			   papermono_app_event_is_touch(event) &&
			   event->x >= remote_mouse_pad.x &&
			   event->x < remote_mouse_pad.x + remote_mouse_pad.width &&
			   event->y >= remote_mouse_pad.y &&
			   event->y < remote_mouse_pad.y + remote_mouse_pad.height) {
			ret = remote_send(remote, PAPERMONO_HID_MOUSE, 0,
				event->x >= 360 ? 0 : 40,
				event->x >= 360 ? -1 : 0, 0);
		}
	}
	if (ret < 0) {
		LOG_WRN("HID action failed (%d)", ret);
		request_redraw(command);
	}
	return 0;
}

#define CALENDAR_ROWS 5U

/* Open the calendar on the page holding the next upcoming event. Events are
 * stored oldest first, so starting at index 0 shows history nobody asked for.
 * Falls back to the first page when the clock is unset or nothing is upcoming.
 */
static size_t calendar_upcoming_page(size_t count)
{
	struct papermono_ui_snapshot snapshot;
	struct papermono_calendar_time now;
	size_t index;

	if (count == 0U) {
		return 0U;
	}
	read_snapshot(&snapshot);
	if (!snapshot.rtc_valid) {
		return 0U;
	}
	now = (struct papermono_calendar_time) {
		.year = snapshot.time.tm_year + 1900,
		.month = snapshot.time.tm_mon + 1,
		.day = snapshot.time.tm_mday,
		.hour = snapshot.time.tm_hour,
		.minute = snapshot.time.tm_min,
		.second = snapshot.time.tm_sec,
	};
	if (papermono_organizer_find_upcoming_index(&now, &index) < 0) {
		return 0U;
	}
	return (index / CALENDAR_ROWS) * CALENDAR_ROWS;
}

static int organizer_open(void *context)
{
	struct organizer_ui_state *ui = context;
	int ret = papermono_organizer_reload();

	if (ret < 0 && ret != -ENODEV) {
		LOG_WRN("Organizer reload reported %d", ret);
	}
	if (ui != NULL) {
		ui->calendar_first =
			calendar_upcoming_page(papermono_organizer_calendar_count());
	}
	return 0;
}

static const char *recurrence_name(enum papermono_recurrence_frequency frequency)
{
	switch (frequency) {
	case PAPERMONO_RECURRENCE_DAILY:
		return "每日";
	case PAPERMONO_RECURRENCE_WEEKLY:
		return "每周";
	case PAPERMONO_RECURRENCE_MONTHLY:
		return "每月";
	case PAPERMONO_RECURRENCE_YEARLY:
		return "每年";
	case PAPERMONO_RECURRENCE_NONE:
	default:
		return NULL;
	}
}

static int render_calendar(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_calendar_count();

	make_header(screen, "日历");
	if (count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 220);
		add_card_text(card, "暂无日程",
			"通过内容中心上传 .ics 文件。支持全天事件、开始/结束时间和基础重复规则。",
			"ICS VEVENT");
	} else {
		if (ui->calendar_first >= count) {
			ui->calendar_first = ((count - 1U) / 5U) * 5U;
		}
		for (size_t row = 0U; row < 5U; ++row) {
			const size_t index = ui->calendar_first + row;
			struct papermono_calendar_event event;
			char body[160];
			char meta[64];
			lv_obj_t *card;

			if (papermono_organizer_get_event(index, &event) < 0) {
				break;
			}
			card = make_card(screen, 22, 92 + (int32_t)row * 126,
				436, 110);
			(void)snprintf(body, sizeof(body),
				event.start.all_day ? "%04d-%02u-%02u · 全天" :
				"%04d-%02u-%02u · %02u:%02u",
				event.start.year, event.start.month, event.start.day,
				event.start.hour, event.start.minute);
			const char *repeat = recurrence_name(event.recurrence);
			if (repeat == NULL) {
				(void)snprintf(meta, sizeof(meta), "EVENT %u / %u",
					(unsigned int)(index + 1U), (unsigned int)count);
			} else {
				(void)snprintf(meta, sizeof(meta), "%s · EVENT %u / %u",
					repeat, (unsigned int)(index + 1U),
					(unsigned int)count);
			}
			add_card_text(card, event.title, body, meta);
		}
	}
	make_footer(screen, "实体键或上下滑动翻页  ·  轻触页脚返回");
	return 0;
}

static int calendar_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_calendar_count();

	if ((event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
	     papermono_app_event_is_button(event)) ||
	    event->type == PAPERMONO_APP_EVENT_DOWN) {
		ui->calendar_first = ui->calendar_first >= 5U ?
			ui->calendar_first - 5U : 0U;
		request_redraw(command);
	} else if ((event->type == PAPERMONO_APP_EVENT_NEXT &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_UP) {
		if (ui->calendar_first + 5U < count) {
			ui->calendar_first += 5U;
		}
		request_redraw(command);
	} else if (event->type == PAPERMONO_APP_EVENT_BACK ||
		   (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_touch(event)) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_FOOTER_TOP)) {
		/* This list is the app root, so back leaves the app. Handling
		 * EVENT_BACK explicitly matters because the edge-swipe gesture
		 * is otherwise swallowed and leaves the user stranded.
		 */
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static const struct papermono_interaction_grid_layout organizer_list_grid = {
	.x = 22,
	.y = 92,
	.cell_width = 436,
	.cell_height = 110,
	.column_gap = 0,
	.row_gap = 16,
	.columns = 1,
	.rows = 5,
};

static int render_tasks(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_task_count();

	make_header(screen, "待办");
	if (count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 220);
		add_card_text(card, "暂无待办",
			"通过内容中心上传 tasks-v1.json；设备只修改独立完成状态，不改写源文件。",
			"TASKS V1");
	} else {
		if (ui->task_first >= count) {
			ui->task_first = ((count - 1U) / 5U) * 5U;
		}
		for (size_t row = 0U; row < 5U; ++row) {
			const size_t index = ui->task_first + row;
			struct papermono_task task;
			char title[150];
			char meta[64];
			lv_obj_t *card;

			if (papermono_organizer_get_task(index, &task) < 0) {
				break;
			}
			card = make_card(screen, 22, 92 + (int32_t)row * 126,
				436, 110);
			(void)snprintf(title, sizeof(title), "%s %s",
				task.done ? "[x]" : "[ ]", task.title);
			(void)snprintf(meta, sizeof(meta), "优先级 %d · %u / %u",
				task.priority, (unsigned int)(index + 1U),
				(unsigned int)count);
			add_card_text(card, title,
				task.due[0] == '\0' ? "无截止日期" : task.due, meta);
		}
	}
	make_footer(screen, "轻触勾选  ·  实体键翻页  ·  页脚返回");
	return 0;
}

static int tasks_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_task_count();

	size_t tapped_row;

	if (papermono_interaction_spaced_grid_index(event,
		PAPERMONO_INTERACTION_TAP, &organizer_list_grid, &tapped_row) == 0) {
		const size_t index = ui->task_first + tapped_row;
		if (index < count) {
			(void)papermono_organizer_toggle_task(index);
			request_redraw(command);
		}
	} else if ((event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_DOWN) {
		ui->task_first = ui->task_first >= 5U ? ui->task_first - 5U : 0U;
		request_redraw(command);
	} else if ((event->type == PAPERMONO_APP_EVENT_NEXT &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_UP) {
		if (ui->task_first + 5U < count) {
			ui->task_first += 5U;
		}
		request_redraw(command);
	} else if (event->type == PAPERMONO_APP_EVENT_BACK ||
		   (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_touch(event)) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_FOOTER_TOP)) {
		/* This list is the app root, so back leaves the app. Handling
		 * EVENT_BACK explicitly matters because the edge-swipe gesture
		 * is otherwise swallowed and leaves the user stranded.
		 */
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static const char *note_block_prefix(enum papermono_note_block_type type)
{
	return type == PAPERMONO_NOTE_LIST_ITEM ? "· " :
		type == PAPERMONO_NOTE_QUOTE ? "│ " : "";
}

static int render_notes(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_note_count();

	make_header(screen, ui->note_open ? "便签内容" : "便签");
	if (ui->note_open &&
	    papermono_organizer_get_note(ui->note_selected, &note_view,
		NULL, NULL) == 0) {
		lv_obj_t *title = make_chinese_label(screen, note_view.title);
		lv_obj_set_pos(title, 24, 94);
		lv_obj_set_width(title, 432);
		int32_t y = 138;
		for (size_t index = 0U; index < note_view.block_count && y < 704;
		     ++index) {
			const struct papermono_note_block *block = &note_view.blocks[index];
			if (block->type == PAPERMONO_NOTE_SEPARATOR) {
				lv_obj_t *line = lv_obj_create(screen);
				lv_obj_set_pos(line, 24, y + 8);
				lv_obj_set_size(line, 432, 2);
				set_box_style(line, ink, 0, 0);
				y += 30;
				continue;
			}
			char text[180];
			(void)snprintf(text, sizeof(text), "%s%s",
				note_block_prefix(block->type), block->text);
			lv_obj_t *label = make_chinese_label(screen, text);
			lv_obj_set_pos(label, block->type == PAPERMONO_NOTE_QUOTE ? 34 : 24,
				y);
			lv_obj_set_width(label, block->type == PAPERMONO_NOTE_QUOTE ? 418 : 432);
			lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
			lv_obj_update_layout(label);
			const int32_t block_height = MAX(lv_obj_get_height(label), 22);
			if (y + block_height > 704) {
				lv_obj_set_height(label, MAX(704 - y, 1));
				y = 704;
			} else {
				y += block_height +
					(block->type == PAPERMONO_NOTE_HEADING ? 14 : 12);
			}
		}
		make_footer(screen, "长按切换收藏  ·  左键或页脚返回列表");
		return 0;
	}
	ui->note_open = false;
	if (count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 132, 436, 220);
		add_card_text(card, "暂无便签",
			"上传 Markdown 或 TXT。仅显示标题、段落、列表、引用和分隔线。",
			"READ ONLY");
	} else {
		if (ui->note_first >= count) {
			ui->note_first = ((count - 1U) / 5U) * 5U;
		}
		for (size_t row = 0U; row < 5U; ++row) {
			const size_t index = ui->note_first + row;
			bool favorite;
			bool read;
			lv_obj_t *card;
			char meta[64];

			if (papermono_organizer_get_note(index, &note_view, &favorite,
				&read) < 0) {
				break;
			}
			card = make_card(screen, 22, 92 + (int32_t)row * 126,
				436, 110);
			(void)snprintf(meta, sizeof(meta), "%s%s · %u / %u",
				favorite ? "收藏 " : "", read ? "已读" : "未读",
				(unsigned int)(index + 1U), (unsigned int)count);
			add_card_text(card, note_view.title,
				note_view.block_count > 0U ? note_view.blocks[0].text :
				"空便签", meta);
		}
	}
	make_footer(screen, "轻触打开  ·  长按收藏  ·  实体键翻页");
	return 0;
}

static int notes_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_note_count();

	if (ui->note_open) {
		if (event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
			bool favorite = false;
			(void)papermono_organizer_get_note(ui->note_selected, &note_view,
				&favorite, NULL);
			(void)papermono_organizer_set_note_favorite(ui->note_selected,
				!favorite);
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_BACK ||
			   event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
			   (event->type == PAPERMONO_APP_EVENT_TAP &&
			    event->y >= UI_FOOTER_TOP)) {
			/* Match the reader's hierarchical return: the edge-swipe
			 * back gesture closes the open note rather than being
			 * swallowed, so it returns to the list instead of
			 * leaving the user stuck on the detail page.
			 */
			ui->note_open = false;
			request_redraw(command);
		}
		return 0;
	}
	size_t tapped_row;
	if (papermono_interaction_spaced_grid_index(event,
		PAPERMONO_INTERACTION_TAP | PAPERMONO_INTERACTION_LONG_PRESS,
		&organizer_list_grid, &tapped_row) == 0) {
		const size_t index = ui->note_first + tapped_row;
		if (index < count) {
			if (event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
				bool favorite = false;
				(void)papermono_organizer_get_note(index, &note_view,
					&favorite, NULL);
				(void)papermono_organizer_set_note_favorite(index,
					!favorite);
			} else {
				ui->note_selected = index;
				ui->note_open = true;
				(void)papermono_organizer_set_note_read(index, true);
			}
			request_redraw(command);
		}
	} else if ((event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_DOWN) {
		ui->note_first = ui->note_first >= 5U ? ui->note_first - 5U : 0U;
		request_redraw(command);
	} else if ((event->type == PAPERMONO_APP_EVENT_NEXT &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_UP) {
		if (ui->note_first + 5U < count) {
			ui->note_first += 5U;
		}
		request_redraw(command);
	} else if (event->type == PAPERMONO_APP_EVENT_BACK ||
		   (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_touch(event)) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_FOOTER_TOP)) {
		/* This list is the app root, so back leaves the app. Handling
		 * EVENT_BACK explicitly matters because the edge-swipe gesture
		 * is otherwise swallowed and leaves the user stranded.
		 */
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static const char *info_status_name(enum papermono_info_status status)
{
	return status == PAPERMONO_INFO_FRESH ? "最新" :
		status == PAPERMONO_INFO_STALE ? "离线缓存" :
		status == PAPERMONO_INFO_ERROR ? "获取失败" : "空";
}

static int render_info(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_info_count();

	make_header(screen, "信息卡片");
	if (count == 0U) {
		lv_obj_t *card = make_card(screen, 22, 112, 436, 194);
		add_card_text(card, "尚未配置数据源",
			"信息槽位已经就绪；确定 RSS 地址、股票市场和数据源后再加载 Provider。",
			"PROVIDER NEUTRAL");
		card = make_card(screen, 22, 330, 436, 154);
		add_card_text(card, "统一数据模型",
			"标题 · 摘要 · 时间 · 来源 · 状态 · 可选数值变化",
			"MODEL V1");
		card = make_card(screen, 22, 508, 436, 154);
		add_card_text(card, "离线行为",
			"网络不可用时保留最后缓存，并明确显示数据更新时间。",
			"NVS CACHE");
	} else {
		if (ui->info_first >= count) {
			ui->info_first = ((count - 1U) / 4U) * 4U;
		}
		for (size_t row = 0U; row < 4U; ++row) {
			const size_t index = ui->info_first + row;
			struct papermono_info_card info;
			char title[140];
			char meta[96];
			lv_obj_t *card;

			if (papermono_info_get(index, &info) < 0) {
				break;
			}
			card = make_card(screen, 22, 92 + (int32_t)row * 158,
				436, 142);
			if (info.has_value) {
				(void)snprintf(title, sizeof(title), "%s · %s",
					info.title, info.value);
			} else {
				(void)snprintf(title, sizeof(title), "%s", info.title);
			}
			(void)snprintf(meta, sizeof(meta), "%s · %s · %lld",
				info.source, info_status_name(info.status),
				(long long)info.updated_at);
			add_card_text(card, title, info.summary, meta);
		}
	}
	make_footer(screen, "Provider 后台更新  ·  实体键翻页  ·  页脚返回");
	return 0;
}

static int info_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_info_count();

	if ((event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
	     papermono_app_event_is_button(event)) ||
	    event->type == PAPERMONO_APP_EVENT_DOWN) {
		ui->info_first = ui->info_first >= 4U ? ui->info_first - 4U : 0U;
		request_redraw(command);
	} else if ((event->type == PAPERMONO_APP_EVENT_NEXT &&
		    papermono_app_event_is_button(event)) ||
		   event->type == PAPERMONO_APP_EVENT_UP) {
		if (ui->info_first + 4U < count) {
			ui->info_first += 4U;
		}
		request_redraw(command);
	} else if (event->type == PAPERMONO_APP_EVENT_BACK ||
		   (event->type == PAPERMONO_APP_EVENT_PREVIOUS &&
		    papermono_app_event_is_touch(event)) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_FOOTER_TOP)) {
		/* This list is the app root, so back leaves the app. Handling
		 * EVENT_BACK explicitly matters because the edge-swipe gesture
		 * is otherwise swallowed and leaves the user stranded.
		 */
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static const char *ready_text(const struct device *device)
{
	return device_is_ready(device) ? "驱动就绪" : "不可用";
}

static const char *status_led_text(enum papermono_status_led_state state)
{
	return state == PAPERMONO_STATUS_LED_OFF ? "熄灭 · 休眠/关机" :
		state == PAPERMONO_STATUS_LED_BOOTING ? "白色 · 启动中" :
		state == PAPERMONO_STATUS_LED_READY ? "绿色 · 就绪" :
		state == PAPERMONO_STATUS_LED_CONNECTING ? "蓝色 · 配网/连接中" :
		state == PAPERMONO_STATUS_LED_CONNECTED ? "青色 · 已连接" :
		state == PAPERMONO_STATUS_LED_BUSY ? "黄色 · 正在传输" :
		state == PAPERMONO_STATUS_LED_PAIRING ? "紫色 · 等待配对" :
		state == PAPERMONO_STATUS_LED_WARNING ? "橙色 · 可恢复警告" :
		"红色 · 系统错误";
}

static int render_tools(void *context, lv_obj_t *screen)
{
	const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
	const struct device *const nfc = DEVICE_DT_GET(DT_NODELABEL(nfc));
	const struct device *const lora = DEVICE_DT_GET(DT_NODELABEL(lora));
	const struct device *const microphone = DEVICE_DT_GET(DT_NODELABEL(microphone));
	struct papermono_sync_state sync = { 0 };
	struct papermono_ft6336_status touch_status = { 0 };
	enum papermono_wakeup_cause wake_cause = PAPERMONO_WAKE_OTHER;
	char body[160];
	lv_obj_t *card;

	ARG_UNUSED(context);
	make_header(screen, "硬件工具");
	const int touch_ret = papermono_ft6336_get_status(&touch_status);
	const int wake_ret = papermono_power_get_wakeup_cause(&wake_cause);
	(void)snprintf(body, sizeof(body), "%s · X %d Y %d · %s\n"
		"帧 %u · I2C %u · 异常 %u · 唤醒 %s",
		touch_ret == 0 && touch_status.online ? "在线" : "等待恢复",
		touch_status.x, touch_status.y,
		touch_status.pressed ? "按下" : "松开", touch_status.frames,
		touch_status.read_errors, touch_status.invalid_frames,
		wake_ret == 0 ? papermono_wakeup_cause_name(wake_cause) : "不可用");
	card = make_card(screen, 22, 88, 436, 112);
	add_card_text(card, "系统、触摸与 TF", body,
		papermono_content_store_state_text(
			papermono_content_store_media_state()));

	int sample = device_is_ready(imu) ? sensor_sample_fetch(imu) : -ENODEV;
	(void)snprintf(body, sizeof(body), "%s · %s",
		ready_text(imu), sample == 0 ? "实时采样成功" : "尚无有效采样");
	card = make_card(screen, 22, 216, 436, 112);
	add_card_text(card, "IMU", body, "BMI270");

	(void)snprintf(body, sizeof(body), "NFC：%s · 未做真实标签 RF 验收\n"
		"LoRa：%s · 默认禁止发射", ready_text(nfc), ready_text(lora));
	card = make_card(screen, 22, 344, 436, 128);
	add_card_text(card, "RF 工具", body, "EXPLICIT HIL ONLY");

	(void)snprintf(body, sizeof(body), "麦克风：%s\n状态灯：%s",
		ready_text(microphone),
		status_led_text(papermono_status_led_get_state()));
	card = make_card(screen, 22, 488, 436, 112);
	add_card_text(card, "音频与提示", body, "RGB STATUS · BUZZER ISOLATED");

#if defined(CONFIG_PAPERMONO_BLE_SYNC)
	papermono_sync_get_state(&sync);
#endif
	if (sync.active) {
		(void)snprintf(body, sizeof(body), "正在接收 %llu / %llu 字节",
			(unsigned long long)sync.received,
			(unsigned long long)sync.expected);
	} else {
		(void)snprintf(body, sizeof(body), "%s · 最近错误 %d",
			sync.initialized ? "认证服务已就绪" : "同步服务未启用",
			sync.last_error);
	}
	card = make_card(screen, 22, 616, 436, 110);
	add_card_text(card, "BLE 小数据同步", body, "NO BOOK TRANSFER");
	make_footer(screen, "轻触首卡查看系统状态  ·  页脚返回");
	return 0;
}

static int tools_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	static const struct papermono_rect system_rect = { 22, 88, 436, 112 };

	ARG_UNUSED(context);
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&system_rect)) {
		(void)papermono_content_store_poll();
		(void)papermono_organizer_reload();
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_SYSTEM;
	} else if (papermono_app_event_is_back_navigation(event) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_FOOTER_TOP)) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static int render_power(void *context, lv_obj_t *screen)
{
	char status[96];

	ARG_UNUSED(context);
	make_header(screen, "电源");
	if (confirmation != CONFIRM_NONE) {
		const char *title = confirmation == CONFIRM_SLEEP ? "确认休眠？" :
			confirmation == CONFIRM_SHUTDOWN ? "确认关机？" :
			confirmation == CONFIRM_RESET_WIFI ? "确认重新配网？" :
			confirmation == CONFIRM_RESET_BLE ? "确认清除蓝牙配对？" :
			"确认恢复默认设置？";
		const char *body = confirmation == CONFIRM_SLEEP ?
			"屏幕将保留当前画面，触摸或电源键唤醒。" :
			confirmation == CONFIRM_SHUTDOWN ?
			"设备将完全关机，仅可通过电源键重新启动。" :
			"此操作会清除对应的本地状态，内容文件不会删除。";
		lv_obj_t *card = make_card(screen, 22, 132, 436, 270);

		add_card_text(card, title, body, "CONFIRMATION REQUIRED");
		card = make_card(screen, 22, 448, 436, 96);
		add_card_text(card, "确认执行", "轻触此处继续", NULL);
		card = make_card(screen, 22, 568, 436, 96);
		add_card_text(card, "取消", "返回上一页", NULL);
		make_footer(screen, "所有电源操作都需要二次确认");
		return 0;
	}

	lv_obj_t *card = make_card(screen, 22, 116, 436, 174);
	add_card_text(card, "立即休眠", "保留当前墨水屏画面；触摸或电源键唤醒。",
		"DEEP SLEEP");
	card = make_card(screen, 22, 316, 436, 174);
	add_card_text(card, "完全关机", "关闭系统电源；使用电源键重新启动。",
		"POWER OFF");
	card = make_card(screen, 22, 516, 436, 154);
	if (power_last_error < 0) {
		(void)snprintf(status, sizeof(status),
			"上次操作未执行（错误 %d）。请松开屏幕后重试。",
			power_last_error);
	} else {
		(void)snprintf(status, sizeof(status),
			"在设置中选择 1 / 5 / 10 / 30 分钟或关闭。");
	}
	add_card_text(card, power_last_error < 0 ? "电源操作已取消" : "自动休眠",
		status, power_last_error < 0 ? "SYSTEM STILL RUNNING" :
		"DEFAULT 10 MIN");
	make_footer(screen, "向右滑动或左键返回");
	return 0;
}

static int render_settings(void *context, lv_obj_t *screen)
{
	const struct papermono_preferences *prefs = papermono_settings_get();
	lv_obj_t *card;
	char value[80];

	ARG_UNUSED(context);
	make_header(screen, "设置");
	(void)snprintf(value, sizeof(value), prefs->idle_sleep_minutes == 0U ?
		"已关闭" : "%u 分钟", prefs->idle_sleep_minutes);
	card = make_card(screen, 22, 88, 436, 84);
	add_card_text(card, "自动休眠", value, "TAP TO CHANGE");
	card = make_card(screen, 22, 180, 436, 84);
	add_card_text(card, "时间格式", prefs->use_24_hour ? "24 小时制" :
		"12 小时制", "ASIA/SHANGHAI");
	(void)snprintf(value, sizeof(value), prefs->frontlight_percent == 0U ?
		"已关闭" : "%u%%", prefs->frontlight_percent);
	card = make_card(screen, 22, 272, 436, 84);
	add_card_text(card, "前光亮度", value, frontlight_last_error < 0 ?
		"设置失败 · 未保存" : "轻触/右滑增加 · 左滑减少 · 长按关闭");
	card = make_card(screen, 22, 364, 436, 84);
	add_card_text(card, "重新配置 Wi-Fi", "清除网络配置并重新进入配网页面", NULL);
	card = make_card(screen, 22, 456, 436, 84);
	add_card_text(card, "清除蓝牙配对", "删除已保存的电脑主机", NULL);
	card = make_card(screen, 22, 548, 436, 84);
	add_card_text(card, "恢复默认设置", "长按后仍需二次确认", NULL);
	card = make_card(screen, 22, 640, 436, 70);
	add_card_text(card, "电源选项", "休眠与关机", NULL);
	if (frontlight_last_error < 0) {
		(void)snprintf(value, sizeof(value),
			"前光调整失败（%d） · 向右滑动返回", frontlight_last_error);
		make_footer(screen, value);
	} else {
		make_footer(screen, "轻触修改  ·  前光长按关闭  ·  向右滑动返回");
	}
	return 0;
}

static int render_system(void *context, lv_obj_t *screen)
{
	struct papermono_ui_snapshot snapshot;
	char body[192];
	char battery[20];
	char input[20];
	char output[20];
	lv_obj_t *card;

	ARG_UNUSED(context);
	read_snapshot(&snapshot);
	format_voltage(battery, sizeof(battery), snapshot.battery_valid,
		snapshot.battery_mv);
	format_voltage(input, sizeof(input), snapshot.input_valid,
		snapshot.input_mv);
	format_voltage(output, sizeof(output), snapshot.output_valid,
		snapshot.output_mv);
	make_header(screen, "系统状态");
	(void)snprintf(body, sizeof(body),
		"Zephyr %s\nLVGL 9 · SSD1677 480×800\n运行时间 %u 分钟",
		KERNEL_VERSION_STRING, snapshot.uptime_minutes);
	card = make_card(screen, 22, 100, 436, 186);
	add_card_text(card, "固件", body, "C153 / ESP32-S3");
	(void)snprintf(body, sizeof(body),
		"电池 %s\n输入 %s\n5V 输出 %s\n电源来源 %s",
		battery, input, output,
		snapshot.power_source_valid ? "可用" : "--");
	card = make_card(screen, 22, 310, 436, 206);
	add_card_text(card, "电源与充电", body, "M5PM1 LIVE DATA");
	card = make_card(screen, 22, 540, 436, 152);
	add_card_text(card, "存储",
		papermono_content_store_state_text(
			papermono_content_store_media_state()),
		"NVS 192 KIB · TF CONTENT");
	make_footer(screen, "向右滑动或左键返回");
	return 0;
}

static void request_redraw(struct papermono_app_command *command)
{
	command->type = PAPERMONO_APP_COMMAND_REDRAW;
	command->refresh_quality = PAPERMONO_REFRESH_AUTO;
}

static void request_fast_redraw(struct papermono_app_command *command)
{
	command->type = PAPERMONO_APP_COMMAND_REDRAW;
	command->refresh_quality = PAPERMONO_REFRESH_FAST;
}

static int library_start_selected(struct library_ui_state *state,
	struct papermono_app_command *command)
{
	if (state->selected >= state->book_count) {
		return -ENOENT;
	}
	state->last_error = papermono_reader_open(&state->reader,
		&state->books[state->selected]);
	if (state->last_error == 0) {
		state->mode = LIBRARY_MODE_READER;
		const int ret = papermono_settings_set_reader_resume(true);

		if (ret < 0) {
			LOG_WRN("Unable to persist reader resume state (%d)", ret);
		}
	}
	request_redraw(command);
	return 0;
}

enum library_touch_action {
	LIBRARY_TOUCH_PREVIOUS_PAGE = 1,
	LIBRARY_TOUCH_NEXT_PAGE,
	LIBRARY_TOUCH_OPEN_MENU,
	LIBRARY_TOUCH_CLOSE_READER,
};

static const struct papermono_interaction_region reader_regions[] = {
	{ { 0, 0, 160, UI_FOOTER_TOP }, LIBRARY_TOUCH_PREVIOUS_PAGE, 0,
		PAPERMONO_INTERACTION_TAP },
	{ { 320, 0, 160, UI_FOOTER_TOP }, LIBRARY_TOUCH_NEXT_PAGE, 0,
		PAPERMONO_INTERACTION_TAP },
	{ { 0, UI_FOOTER_TOP, UI_WIDTH, UI_HEIGHT - UI_FOOTER_TOP },
		LIBRARY_TOUCH_CLOSE_READER, 0, PAPERMONO_INTERACTION_TAP },
	{ { 160, 0, 160, UI_FOOTER_TOP }, LIBRARY_TOUCH_OPEN_MENU, 0,
		PAPERMONO_INTERACTION_TAP },
};

static const struct papermono_interaction_grid_layout library_book_grid = {
	.x = 22,
	.y = 116,
	.cell_width = 436,
	.cell_height = 102,
	.column_gap = 0,
	.row_gap = 18,
	.columns = 1,
	.rows = LIBRARY_VISIBLE_BOOKS,
};

static const struct papermono_interaction_grid_layout library_menu_grid = {
	.x = 22,
	.y = 86,
	.cell_width = 436,
	.cell_height = 84,
	.column_gap = 0,
	.row_gap = 8,
	.columns = 1,
	.rows = 7,
};

static const struct papermono_interaction_grid_layout library_auxiliary_grid = {
	.x = 22,
	.y = 96,
	.cell_width = 436,
	.cell_height = 106,
	.column_gap = 0,
	.row_gap = 18,
	.columns = 1,
	.rows = 5,
};

static int library_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct library_ui_state *state = context;

	if (event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED &&
	    state->mode == LIBRARY_MODE_LIST) {
		(void)library_open(state);
		request_redraw(command);
		return 0;
	}

	if (state->mode == LIBRARY_MODE_LIST) {
		static const struct papermono_rect empty_rescan_rect = {
			22, 132, 436, 260,
		};
		size_t tapped_row;

		if (event->type == PAPERMONO_APP_EVENT_BACK) {
			command->type = PAPERMONO_APP_COMMAND_BACK;
		} else if ((state->book_count == 0U &&
			    papermono_interaction_contains(event,
				    PAPERMONO_INTERACTION_LONG_PRESS,
				    &empty_rescan_rect)) ||
			   (state->book_count > 0U &&
			    papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_LONG_PRESS, &library_book_grid,
			&tapped_row) == 0)) {
			(void)library_open(state);
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS) {
			if (state->selected > 0U) {
				--state->selected;
				request_redraw(command);
			} else {
				command->type = PAPERMONO_APP_COMMAND_BACK;
			}
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT) {
			if (state->selected + 1U < state->book_count) {
				++state->selected;
				request_redraw(command);
			}
		} else if (state->book_count > 0U &&
			   papermono_interaction_spaced_grid_index(event,
				   PAPERMONO_INTERACTION_TAP, &library_book_grid,
				   &tapped_row) == 0) {
			const size_t first =
				(state->selected / LIBRARY_VISIBLE_BOOKS) *
				LIBRARY_VISIBLE_BOOKS;
			const size_t selected = first + tapped_row;

			if (selected < state->book_count) {
				state->selected = selected;
				return library_start_selected(state, command);
			}
		}
		return 0;
	}

	if (state->mode == LIBRARY_MODE_MENU) {
		size_t menu_item;
		struct papermono_rect frontlight_rect;
		const bool menu_hit = papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_TAP | PAPERMONO_INTERACTION_LONG_PRESS,
			&library_menu_grid, &menu_item) == 0;
		const bool frontlight_rect_valid =
			papermono_interaction_grid_cell_rect(&library_menu_grid, 3U,
				&frontlight_rect) == 0;
		const bool frontlight_swipe = frontlight_rect_valid &&
			papermono_app_event_is_touch(event) &&
			event->x >= frontlight_rect.x &&
			event->x < frontlight_rect.x + frontlight_rect.width &&
			event->y >= frontlight_rect.y &&
			event->y < frontlight_rect.y + frontlight_rect.height &&
			(event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
			 event->type == PAPERMONO_APP_EVENT_NEXT);

		if (event->type == PAPERMONO_APP_EVENT_BACK) {
			state->mode = LIBRARY_MODE_READER;
			request_redraw(command);
		} else if (menu_hit && menu_item == 0U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			(void)papermono_reader_toggle_bookmark(&state->reader,
				state->page_text);
			request_redraw(command);
		} else if (menu_hit && menu_item == 1U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			if (state->reader.book.kind == PAPERMONO_CONTENT_EPUB) {
				state->auxiliary_selected = 0U;
				for (uint16_t toc = 0U;
				     toc < state->reader.epub.toc_count; ++toc) {
					if (state->reader.epub.toc[toc].chapter ==
					    state->reader.chapter) {
						state->auxiliary_selected = toc;
						break;
					}
				}
				state->mode = LIBRARY_MODE_TOC;
				request_redraw(command);
			}
		} else if (menu_hit && menu_item == 2U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			const enum papermono_reader_density density =
				papermono_settings_get()->reader_density;
			(void)papermono_settings_set_reader_density(
				(enum papermono_reader_density)((density + 1U) % 3U));
			state->reader.page_loaded = false;
			request_redraw(command);
		} else if (menu_hit && menu_item == 2U &&
			   event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
			if (state->reader.book.kind == PAPERMONO_CONTENT_TXT) {
				(void)papermono_reader_set_encoding(&state->reader,
					state->reader.encoding ==
						PAPERMONO_TEXT_ENCODING_UTF8 ?
						PAPERMONO_TEXT_ENCODING_GBK :
						PAPERMONO_TEXT_ENCODING_UTF8);
			}
			request_redraw(command);
		} else if (menu_hit && menu_item == 3U) {
			const uint8_t next = event->type ==
				PAPERMONO_APP_EVENT_LONG_PRESS ? 0U :
				papermono_frontlight_next(
					papermono_settings_get()->frontlight_percent);

			(void)set_frontlight(next);
			request_redraw(command);
		} else if (frontlight_swipe) {
			const uint8_t current =
				papermono_settings_get()->frontlight_percent;
			const uint8_t next = event->type ==
				PAPERMONO_APP_EVENT_PREVIOUS ?
				papermono_frontlight_next(current) :
				papermono_frontlight_previous(current);

			(void)set_frontlight(next);
			request_redraw(command);
		} else if (menu_hit && menu_item == 4U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			state->auxiliary_selected = 0U;
			state->mode = LIBRARY_MODE_BOOKMARKS;
			request_redraw(command);
		} else if (menu_hit && menu_item == 5U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			state->mode = LIBRARY_MODE_READER;
			request_redraw(command);
		} else if (menu_hit && menu_item == 6U &&
			   event->type == PAPERMONO_APP_EVENT_TAP) {
			library_return_to_list(state);
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS) {
			state->mode = LIBRARY_MODE_READER;
			request_redraw(command);
		}
		return 0;
	}

	if (state->mode == LIBRARY_MODE_TOC ||
	    state->mode == LIBRARY_MODE_BOOKMARKS) {
		size_t tapped_row;
		const uint16_t count = state->mode == LIBRARY_MODE_TOC ?
			(state->reader.epub.toc_count > 0U ?
			 state->reader.epub.toc_count : state->reader.epub.chapter_count) :
			state->reader.bookmark_count;

		if (event->type == PAPERMONO_APP_EVENT_BACK) {
			state->mode = LIBRARY_MODE_MENU;
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS) {
			if (state->auxiliary_selected > 0U) {
				--state->auxiliary_selected;
			} else {
				state->mode = LIBRARY_MODE_MENU;
			}
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_NEXT) {
			if (state->auxiliary_selected + 1U < count) {
				++state->auxiliary_selected;
				request_redraw(command);
			}
		} else if (count > 0U &&
			   papermono_interaction_spaced_grid_index(event,
				   PAPERMONO_INTERACTION_TAP, &library_auxiliary_grid,
				   &tapped_row) == 0) {
			const uint16_t first = (state->auxiliary_selected / 5U) * 5U;
			const uint16_t selected = first + (uint16_t)tapped_row;

			if (selected < count) {
				if (state->mode == LIBRARY_MODE_TOC) {
					const struct papermono_epub_toc_entry *toc =
						state->reader.epub.toc_count > 0U ?
						&state->reader.epub.toc[selected] : NULL;
					const uint8_t level = toc == NULL ? 0U : toc->level;
					const int32_t indent = MIN(level, 4U) * 16;

					/* Hierarchy indentation is paper, not a hidden
					 * extension of the visible chapter card.
					 */
					if (event->x < library_auxiliary_grid.x + indent) {
						return 0;
					}
					const uint16_t chapter = toc == NULL ? selected :
						toc->chapter;
					(void)papermono_reader_jump_to_chapter(
						&state->reader, chapter);
				} else {
					(void)papermono_reader_jump_to_bookmark(
						&state->reader, selected);
				}
				state->mode = LIBRARY_MODE_READER;
			}
			request_redraw(command);
		}
		return 0;
	}

	uint16_t touch_action = 0U;
	const bool touch_routed = papermono_interaction_route(event,
		reader_regions, ARRAY_SIZE(reader_regions), &touch_action, NULL) == 0;

	if (event->type == PAPERMONO_APP_EVENT_BACK) {
		library_return_to_list(state);
		request_redraw(command);
	} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
	    (touch_routed && touch_action == LIBRARY_TOUCH_PREVIOUS_PAGE)) {
		const uint32_t old_offset = state->reader.page_start;
		const uint16_t old_chapter = state->reader.chapter;
		const int ret = papermono_reader_previous(&state->reader);

		if (state->reader.page_start != old_offset ||
		    state->reader.chapter != old_chapter) {
			request_fast_redraw(command);
		} else if (ret < 0 && ret != -ENOENT) {
			LOG_WRN("Unable to move to the previous reader page (%d)", ret);
		}
	} else if (event->type == PAPERMONO_APP_EVENT_NEXT ||
		   (touch_routed && touch_action == LIBRARY_TOUCH_NEXT_PAGE)) {
		const uint32_t old_offset = state->reader.page_start;
		const uint16_t old_chapter = state->reader.chapter;
		const int ret = papermono_reader_next(&state->reader);

		if (state->reader.page_start != old_offset ||
		    state->reader.chapter != old_chapter) {
			request_fast_redraw(command);
		} else if (ret < 0 && ret != -ENOENT) {
			LOG_WRN("Unable to move to the next reader page (%d)", ret);
		}
	} else if (event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
		(void)papermono_reader_toggle_bookmark(&state->reader,
			state->page_text);
		request_redraw(command);
	} else if (touch_routed && touch_action == LIBRARY_TOUCH_CLOSE_READER) {
		library_return_to_list(state);
		request_redraw(command);
	} else if (touch_routed && touch_action == LIBRARY_TOUCH_OPEN_MENU) {
		state->mode = LIBRARY_MODE_MENU;
		request_redraw(command);
	}
	return 0;
}

static int home_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	uint16_t action;

	ARG_UNUSED(context);
	if (event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
	    event->type == PAPERMONO_APP_EVENT_NEXT) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_LAUNCHER;
	} else if (papermono_interaction_route(event, home_regions,
			ARRAY_SIZE(home_regions), &action, NULL) == 0) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = (enum papermono_app_id)action;
	}
	return 0;
}

static int launcher_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	const size_t page_count =
		(ARRAY_SIZE(launcher_items) + LAUNCHER_PAGE_SIZE - 1U) /
		LAUNCHER_PAGE_SIZE;
	size_t index;

	ARG_UNUSED(context);
	if (event->type == PAPERMONO_APP_EVENT_BACK) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
	} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
		   event->type == PAPERMONO_APP_EVENT_UP) {
		if (launcher_page > 0U) {
			--launcher_page;
			request_redraw(command);
		}
	} else if (event->type == PAPERMONO_APP_EVENT_NEXT ||
		   event->type == PAPERMONO_APP_EVENT_DOWN) {
		if (launcher_page + 1U < page_count) {
			++launcher_page;
			request_redraw(command);
		}
	} else if (papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_TAP, &launcher_grid, &index) == 0) {
		index += launcher_page * LAUNCHER_PAGE_SIZE;
		if (index < ARRAY_SIZE(launcher_items)) {
			command->type = PAPERMONO_APP_COMMAND_OPEN;
			command->target = launcher_items[index].id;
		}
	}
	return 0;
}

static int weread_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct weread_ui_state *state = context;
	struct papermono_weread_status status;
	enum papermono_weread_navigation navigation;
	enum papermono_weread_navigation_result result;
	const struct papermono_rect footer = {
		0, UI_FOOTER_TOP, UI_WIDTH, UI_HEIGHT - UI_FOOTER_TOP,
	};

	if (state == NULL || event == NULL || command == NULL) {
		return -EINVAL;
	}
	if (event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED ||
	    event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
		state->last_error = papermono_weread_reload();
		papermono_weread_get_status(&status);
		if (status.last_error < 0 || status.count == 0U) {
			papermono_weread_view_reset(&state->view);
		} else if (state->view.selected >= status.count) {
			state->view.selected = status.count - 1U;
		}
		request_redraw(command);
		return 0;
	}

	papermono_weread_get_status(&status);
	state->last_error = status.last_error;
	if ((status.last_error < 0 || status.count == 0U) &&
	    papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&(struct papermono_rect) { 22, 294, 436, 190 })) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_CONTENT;
		return 0;
	}

	if (event->type == PAPERMONO_APP_EVENT_BACK ||
	    papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&footer)) {
		navigation = PAPERMONO_WEREAD_NAV_BACK;
	} else if (event->type == PAPERMONO_APP_EVENT_PREVIOUS) {
		navigation = PAPERMONO_WEREAD_NAV_PREVIOUS;
	} else if (event->type == PAPERMONO_APP_EVENT_NEXT) {
		navigation = PAPERMONO_WEREAD_NAV_NEXT;
	} else if (event->type == PAPERMONO_APP_EVENT_UP) {
		navigation = PAPERMONO_WEREAD_NAV_PAGE_PREVIOUS;
	} else if (event->type == PAPERMONO_APP_EVENT_DOWN) {
		navigation = PAPERMONO_WEREAD_NAV_PAGE_NEXT;
	} else if (event->type == PAPERMONO_APP_EVENT_TAP &&
		   state->view.mode == PAPERMONO_WEREAD_VIEW_LIST) {
		size_t row;

		if (papermono_interaction_spaced_grid_index(event,
			PAPERMONO_INTERACTION_TAP, &weread_book_grid, &row) < 0) {
			return 0;
		}
		const size_t selected = papermono_weread_view_first(&state->view) + row;

		if (selected >= status.count) {
			return 0;
		}
		state->view.selected = selected;
		navigation = PAPERMONO_WEREAD_NAV_OPEN;
	} else {
		return 0;
	}

	int ret = papermono_weread_view_navigate(&state->view, status.count,
		navigation, &result);

	if (ret < 0) {
		return ret;
	}
	if (result == PAPERMONO_WEREAD_NAVIGATION_REDRAW) {
		request_redraw(command);
	} else if (result == PAPERMONO_WEREAD_NAVIGATION_ROUTER_BACK) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static int common_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	const struct papermono_rect footer = {
		0, UI_FOOTER_TOP, UI_WIDTH, UI_HEIGHT - UI_FOOTER_TOP,
	};

	ARG_UNUSED(context);
	if (papermono_app_event_is_back_navigation(event) ||
	    papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&footer)) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static int power_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	static const struct papermono_rect confirm_rect = { 22, 448, 436, 96 };
	static const struct papermono_rect cancel_rect = { 22, 568, 436, 96 };
	static const struct papermono_rect sleep_rect = { 22, 116, 436, 174 };
	static const struct papermono_rect shutdown_rect = { 22, 316, 436, 174 };

	ARG_UNUSED(context);
	if (confirmation != CONFIRM_NONE) {
		if (papermono_interaction_contains(event,
			PAPERMONO_INTERACTION_TAP, &confirm_rect)) {
			if (confirmation == CONFIRM_SLEEP) {
				command->type = PAPERMONO_APP_COMMAND_SLEEP;
			} else if (confirmation == CONFIRM_SHUTDOWN) {
				command->type = PAPERMONO_APP_COMMAND_SHUTDOWN;
			} else if (confirmation == CONFIRM_RESET_SETTINGS) {
				const int reset_ret = papermono_settings_reset();

				if (reset_ret < 0) {
					frontlight_last_error = reset_ret;
				} else {
					frontlight_last_error = apply_frontlight();
				}
				confirmation = CONFIRM_NONE;
				request_redraw(command);
			} else if (confirmation == CONFIRM_RESET_WIFI) {
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
				int ret = papermono_content_server_reset_wifi();

				if (ret < 0 && ret != -EALREADY) {
					LOG_ERR("Unable to reset Wi-Fi (%d)", ret);
				}
#else
				(void)papermono_settings_clear_wifi();
#endif
				confirmation = CONFIRM_NONE;
				command->type = PAPERMONO_APP_COMMAND_OPEN;
				command->target = PAPERMONO_APP_CONTENT;
			} else if (confirmation == CONFIRM_RESET_BLE) {
#if defined(CONFIG_PAPERMONO_BLE_HID)
				int ret = papermono_hid_clear_bonds();

				if (ret < 0 && ret != -ENOENT) {
					LOG_ERR("Unable to clear BLE bonds (%d)", ret);
				}
#endif
				confirmation = CONFIRM_NONE;
				command->type = PAPERMONO_APP_COMMAND_OPEN;
				command->target = PAPERMONO_APP_REMOTE;
			} else {
				LOG_INF("Confirmed state cleanup request %d", confirmation);
				confirmation = CONFIRM_NONE;
				request_redraw(command);
			}
		} else if (papermono_app_event_is_back_navigation(event) ||
			   papermono_interaction_contains(event,
				   PAPERMONO_INTERACTION_TAP, &cancel_rect)) {
			confirmation = CONFIRM_NONE;
			request_redraw(command);
		}
		return 0;
	}
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&sleep_rect)) {
		confirmation = CONFIRM_SLEEP;
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &shutdown_rect)) {
		confirmation = CONFIRM_SHUTDOWN;
		request_redraw(command);
	} else {
		return common_event(context, event, command);
	}
	return 0;
}

static void power_close(void *context)
{
	ARG_UNUSED(context);
	/* HOME bypasses page event handling and routes directly. Clear any
	 * unfinished confirmation whenever the power page loses ownership so a
	 * later visit can never resurrect a stale destructive action.
	 */
	confirmation = CONFIRM_NONE;
}

static int settings_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	static const struct papermono_rect sleep_rect = { 22, 88, 436, 84 };
	static const struct papermono_rect time_rect = { 22, 180, 436, 84 };
	static const struct papermono_rect frontlight_rect = { 22, 272, 436, 84 };
	static const struct papermono_rect wifi_rect = { 22, 364, 436, 84 };
	static const struct papermono_rect ble_rect = { 22, 456, 436, 84 };
	static const struct papermono_rect reset_rect = { 22, 548, 436, 84 };
	static const struct papermono_rect power_rect = { 22, 640, 436, 70 };
	const struct papermono_preferences *prefs = papermono_settings_get();

	ARG_UNUSED(context);
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&sleep_rect)) {
		static const uint8_t choices[] = { 0U, 1U, 5U, 10U, 30U };
		size_t index = 0U;

		while (index < ARRAY_SIZE(choices) &&
		       choices[index] != prefs->idle_sleep_minutes) {
			++index;
		}
		index = (index + 1U) % ARRAY_SIZE(choices);
		(void)papermono_settings_set_idle_sleep(choices[index]);
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &time_rect)) {
		(void)papermono_settings_set_time_format(!prefs->use_24_hour);
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP | PAPERMONO_INTERACTION_LONG_PRESS,
		&frontlight_rect)) {
		const uint8_t next = event->type == PAPERMONO_APP_EVENT_LONG_PRESS ?
			0U : papermono_frontlight_next(prefs->frontlight_percent);

		(void)set_frontlight(next);
		request_redraw(command);
	} else if (papermono_app_event_is_touch(event) &&
		   event->x >= frontlight_rect.x &&
		   event->x < frontlight_rect.x + frontlight_rect.width &&
		   event->y >= frontlight_rect.y &&
		   event->y < frontlight_rect.y + frontlight_rect.height &&
		   (event->type == PAPERMONO_APP_EVENT_PREVIOUS ||
		    event->type == PAPERMONO_APP_EVENT_NEXT)) {
		const uint8_t next = event->type ==
			PAPERMONO_APP_EVENT_PREVIOUS ?
			papermono_frontlight_next(prefs->frontlight_percent) :
			papermono_frontlight_previous(prefs->frontlight_percent);

		(void)set_frontlight(next);
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &wifi_rect)) {
		confirmation = CONFIRM_RESET_WIFI;
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &ble_rect)) {
		confirmation = CONFIRM_RESET_BLE;
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_LONG_PRESS, &reset_rect)) {
		confirmation = CONFIRM_RESET_SETTINGS;
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &power_rect)) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
	} else {
		return common_event(context, event, command);
	}
	return 0;
}

static const struct papermono_app_descriptor apps[] = {
	{ PAPERMONO_APP_HOME, "首页", true, NULL, render_home, home_event, NULL, NULL },
	{ PAPERMONO_APP_LAUNCHER, "全部应用", true, NULL, render_launcher, launcher_event, NULL, NULL },
	{ PAPERMONO_APP_LIBRARY, "书库", true, library_open, render_library, library_event, library_close, &library_state },
	{ PAPERMONO_APP_WEREAD, "微信读书", true, weread_open, render_weread, weread_event, NULL, &weread_ui },
	{ PAPERMONO_APP_CONTENT, "内容中心", true, NULL, render_content_center, common_event, NULL, NULL },
	{ PAPERMONO_APP_REMOTE, "遥控器", true, NULL, render_remote, remote_event, NULL, &remote_state },
	{ PAPERMONO_APP_CALENDAR, "日历", true, organizer_open, render_calendar, calendar_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_TASKS, "待办", true, organizer_open, render_tasks, tasks_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_NOTES, "便签", true, organizer_open, render_notes, notes_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_INFO, "信息卡片", true, NULL, render_info, info_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_TOOLS, "硬件工具", true, NULL, render_tools, tools_event, NULL, NULL },
	{ PAPERMONO_APP_POWER, "电源", true, NULL, render_power, power_event, power_close, NULL },
	{ PAPERMONO_APP_SETTINGS, "设置", true, NULL, render_settings, settings_event, NULL, NULL },
	{ PAPERMONO_APP_SYSTEM, "系统状态", true, NULL, render_system, common_event, NULL, NULL },
};

#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
static int queue_event(enum papermono_app_event_type type,
	enum papermono_app_event_source source, int16_t x, int16_t y,
	uint32_t duration_ms)
{
	const struct papermono_app_event event = {
		.type = type,
		.source = source,
		.x = x,
		.y = y,
		.duration_ms = duration_ms,
	};

	return k_msgq_put(&ui_events, &event, K_NO_WAIT);
}

static void queue_pending_system_events(void)
{
	if (atomic_cas(&pending_content_refresh, UI_SYSTEM_EVENT_PENDING,
		UI_SYSTEM_EVENT_QUEUED)) {
		/* A content change also redraws connection/status information. */
		(void)atomic_cas(&pending_background_refresh,
			UI_SYSTEM_EVENT_PENDING, 0);
		if (queue_event(PAPERMONO_APP_EVENT_CONTENT_CHANGED,
			PAPERMONO_APP_EVENT_SOURCE_SYSTEM, 0, 0, 0U) < 0) {
			(void)atomic_cas(&pending_content_refresh,
				UI_SYSTEM_EVENT_QUEUED, UI_SYSTEM_EVENT_PENDING);
		}
	} else if (atomic_cas(&pending_background_refresh,
			   UI_SYSTEM_EVENT_PENDING, UI_SYSTEM_EVENT_QUEUED) &&
		   queue_event(PAPERMONO_APP_EVENT_REFRESH,
			PAPERMONO_APP_EVENT_SOURCE_SYSTEM, 0, 0, 0U) < 0) {
		(void)atomic_cas(&pending_background_refresh,
			UI_SYSTEM_EVENT_QUEUED, UI_SYSTEM_EVENT_PENDING);
	}
}

static void papermono_ui_input_event(struct input_event *event,
	void *user_data)
{
	ARG_UNUSED(user_data);
	if (event->dev == buttons && event->type == INPUT_EV_KEY &&
	    event->value != 0) {
		if (atomic_get(&input_blocked) != 0) {
			return;
		}
		if (event->code == INPUT_KEY_0) {
			queue_event(PAPERMONO_APP_EVENT_PREVIOUS,
				PAPERMONO_APP_EVENT_SOURCE_BUTTON, -1, -1, 0U);
		} else if (event->code == INPUT_KEY_1) {
			queue_event(PAPERMONO_APP_EVENT_NEXT,
				PAPERMONO_APP_EVENT_SOURCE_BUTTON, -1, -1, 0U);
		}
		return;
	}
	if (event->dev != touch) {
		return;
	}
	if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_X) {
		touch_x = CLAMP(event->value, 0, UI_WIDTH - 1);
	} else if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_Y) {
		touch_y = CLAMP(event->value, 0, UI_HEIGHT - 1);
	} else if (event->type == INPUT_EV_KEY &&
		   event->code == INPUT_BTN_TOUCH) {
		if (event->value != 0) {
			touch_pressed = true;
			touch_start_x = touch_x;
			touch_start_y = touch_y;
			touch_start_ms = k_uptime_get_32();
			LOG_INF("UI touch press x=%d y=%d blocked=%d", touch_start_x,
				touch_start_y, (int)atomic_get(&input_blocked));
		} else if (touch_pressed) {
			const uint32_t now = k_uptime_get_32();
			const uint32_t duration = now - touch_start_ms;
			enum papermono_app_event_type type;
			int16_t event_x;
			int16_t event_y;
			struct papermono_app_event completed;
			int ret;

			touch_pressed = false;
			if (now - last_touch_action_ms < UI_TOUCH_ACTION_COOLDOWN_MS) {
				LOG_INF("UI touch ignored by %u ms action cooldown",
					UI_TOUCH_ACTION_COOLDOWN_MS);
				return;
			}
			ret = papermono_touch_gesture_classify(touch_start_x,
				touch_start_y, touch_x, touch_y, duration,
				UI_WIDTH, UI_HEIGHT, &type);
			if (ret < 0) {
				LOG_WRN("UI touch gesture rejected: start=%d,%d end=%d,%d duration=%u (%d)",
					touch_start_x, touch_start_y, touch_x, touch_y,
					duration, ret);
				return;
			}
			ret = papermono_touch_gesture_event_point(type, touch_start_x,
				touch_start_y, touch_x, touch_y, &event_x, &event_y);
			if (ret < 0) {
				LOG_WRN("UI touch event point rejected (%d)", ret);
				return;
			}
			completed = (struct papermono_app_event) {
				.type = type,
				.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
				.x = event_x,
				.y = event_y,
				.duration_ms = duration,
			};
			if (atomic_get(&input_blocked) != 0) {
				k_spinlock_key_t key =
					k_spin_lock(&deferred_touch_lock);
				bool retained = false;
				bool still_blocked;

				/* Match CrossMux's input-snapshot behavior: retain the
				 * first complete gesture while the e-paper task is busy.
				 */
				still_blocked = atomic_get(&input_blocked) != 0;
				if (still_blocked && !deferred_touch_valid) {
					deferred_touch_event = completed;
					deferred_touch_valid = true;
					retained = true;
				}
				k_spin_unlock(&deferred_touch_lock, key);
				if (retained) {
					LOG_INF("UI touch gesture type=%d x=%d y=%d duration=%u retained during refresh",
						type, event_x, event_y, duration);
					last_touch_action_ms = now;
					return;
				}
				if (still_blocked) {
					/* One complete gesture is already retained for this
					 * refresh. Coalesce later gestures instead of allowing
					 * them to bypass the display input guard.
					 */
					LOG_INF("UI touch gesture coalesced during refresh");
					last_touch_action_ms = now;
					return;
				}
				/* The refresh may have completed while this callback waited for
				 * deferred_touch_lock. In that case the gesture is current and
				 * must fall through to the normal queue instead of being lost.
				 */
			}
			ret = k_msgq_put(&ui_events, &completed, K_NO_WAIT);
			LOG_INF("UI touch gesture type=%d x=%d y=%d duration=%u queue=%d",
				type, event_x, event_y, duration, ret);
			last_touch_action_ms = now;
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, papermono_ui_input_event, NULL);

static void display_input_guard(bool blocked, void *context)
{
	ARG_UNUSED(context);
	if (blocked) {
		atomic_set(&input_blocked, 1);
		return;
	}
	/* Preserve every already completed input event. The old queue purge raced
	 * an input callback that had just observed the refresh guard and could
	 * discard the exact gesture which should open the next page.
	 */
	struct papermono_app_event deferred;
	bool have_deferred;
	k_spinlock_key_t key = k_spin_lock(&deferred_touch_lock);

	atomic_clear(&input_blocked);
	have_deferred = deferred_touch_valid;
	deferred = deferred_touch_event;
	deferred_touch_valid = false;
	k_spin_unlock(&deferred_touch_lock, key);
	if (have_deferred) {
		const int ret = k_msgq_put(&ui_events, &deferred, K_NO_WAIT);

		if (ret < 0) {
			LOG_ERR("Unable to restore deferred touch gesture (%d)", ret);
		}
	}
	queue_pending_system_events();
}
#else
static void display_input_guard(bool blocked, void *context)
{
	ARG_UNUSED(blocked);
	ARG_UNUSED(context);
}
#endif

static int render_current(enum papermono_refresh_quality quality)
{
	lv_obj_t *screen = lv_screen_active();
	int ret;

	/* Page handlers and on_open hooks may have just polled TF/network state.
	 * Submit the LED snapshot only after those mutations, otherwise the worker
	 * can race ahead and leave the physical indicator one event behind.
	 */
	papermono_status_led_request_refresh();
	clock_label = NULL;
	footer_label = NULL;
	lv_obj_clean(screen);
	ui_fixed_line_count = 0U;
	set_box_style(screen, paper, 0, 0);
	ret = papermono_app_router_render(&app_router, screen);
	if (ret < 0) {
		return ret;
	}
	ret = papermono_display_refresh(quality);
	last_refresh_ms = k_uptime_get();
	return ret;
}

static int open_app(enum papermono_app_id target)
{
	int ret = papermono_app_router_open(&app_router, target);

	if (ret < 0) {
		return ret;
	}
	ret = papermono_settings_set_active_app(target);
	if (ret < 0) {
		LOG_WRN("Unable to persist current app %d (%d)", target, ret);
	}
	return render_current(PAPERMONO_REFRESH_AUTO);
}

static int enter_power_state(bool shutdown)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *pool;
	lv_obj_t *label;
	int ret;

	confirmation = CONFIRM_NONE;
	if (!shutdown) {
		ret = papermono_power_touch_wake_ready(1000U);
		if (ret < 0) {
			power_last_error = ret;
			LOG_WRN("Touch wake source is not ready; sleep cancelled (%d)",
				ret);
			return render_current(PAPERMONO_REFRESH_AUTO);
		}
	}
	power_last_error = 0;
	lv_obj_clean(screen);
	ui_fixed_line_count = 0U;
	set_box_style(screen, paper, 0, 0);
	pool = make_outline_shape(screen, 86, 292, 308, 156, 3, 0);
	(void)make_ink_shape(pool, 20, 0, 72, 6, ink, 0);
	label = make_label(pool, shutdown ? "设备已关机" : "设备休眠中",
		&papermono_cjk_16, ink);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -24);
	label = make_label(pool, shutdown ? "按电源键重新启动" :
		"触摸屏幕或按电源键唤醒", &papermono_cjk_16, ink);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, 244);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 22);
	ret = papermono_display_refresh(PAPERMONO_REFRESH_FULL);
	if (ret < 0) {
		return ret;
	}
	ret = shutdown ? papermono_power_shutdown_button() :
		papermono_power_enter_touch_deep_sleep();
	if (ret >= 0) {
		ret = -EIO;
	}
	power_last_error = ret;
	/* A failed low-power transition returns after the power layer has already
	 * turned the frontlight off. Restore the saved value before redrawing.
	 */
	frontlight_last_error = apply_frontlight();
	if (frontlight_last_error < 0) {
		LOG_WRN("Unable to restore frontlight after cancelled power transition (%d)",
			frontlight_last_error);
	}
	papermono_status_led_resume();
	LOG_ERR("Power transition returned without entering low power (%d); "
		"restoring the UI", ret);
	return render_current(PAPERMONO_REFRESH_FULL);
}

static int __maybe_unused process_event(
	const struct papermono_app_event *event)
{
	struct papermono_app_command command;
	const bool background_refresh = event->type == PAPERMONO_APP_EVENT_REFRESH ||
		event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED;
	if (event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED) {
		(void)papermono_organizer_reload();
	}
	int ret;

	if (event->type == PAPERMONO_APP_EVENT_HOME) {
		ret = papermono_app_router_open(&app_router, PAPERMONO_APP_HOME);
		if (ret == 0) {
			/* Home is a stack reset, not another page to return from. */
			app_router.has_previous = false;
			(void)papermono_settings_set_active_app(PAPERMONO_APP_HOME);
			ret = render_current(PAPERMONO_REFRESH_AUTO);
		}
		return ret;
	}
	ret = papermono_app_router_dispatch(&app_router, event, &command);

	if (ret < 0) {
		return ret;
	}
	if (background_refresh && command.type == PAPERMONO_APP_COMMAND_NONE) {
		command.type = PAPERMONO_APP_COMMAND_REDRAW;
	}
	if (event->type == PAPERMONO_APP_EVENT_BACK &&
	    command.type == PAPERMONO_APP_COMMAND_NONE) {
		command.type = PAPERMONO_APP_COMMAND_BACK;
	}
	switch (command.type) {
	case PAPERMONO_APP_COMMAND_OPEN:
		return open_app(command.target);
	case PAPERMONO_APP_COMMAND_BACK:
		ret = papermono_app_router_back(&app_router);
		if (ret == 0) {
			(void)papermono_settings_set_active_app(app_router.current);
			ret = render_current(PAPERMONO_REFRESH_AUTO);
		}
		return ret;
	case PAPERMONO_APP_COMMAND_SLEEP:
		return enter_power_state(false);
	case PAPERMONO_APP_COMMAND_SHUTDOWN:
		return enter_power_state(true);
	case PAPERMONO_APP_COMMAND_REDRAW:
		return render_current(command.refresh_quality);
	case PAPERMONO_APP_COMMAND_NONE:
	default:
		return 0;
	}
}

static bool __maybe_unused automatic_sleep_inhibited(void)
{
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	/* An idle provisioning AP must still obey the configured sleep timeout.
	 * Only defer sleep while a bounded request owns mutable upload/configuration
	 * state; waking the device brings the AP back for later provisioning.
	 */
	return papermono_content_server_busy();
#else
	return false;
#endif
}

int papermono_ui_init(const struct device *display)
{
	enum papermono_app_id initial = PAPERMONO_APP_HOME;
	int ret;

	if (display == NULL || !device_is_ready(display)) {
		return -ENODEV;
	}
	ret = papermono_settings_init();
	if (ret < 0) {
		LOG_WRN("Using volatile default settings (%d)", ret);
	}
	ret = papermono_content_store_init();
	if (ret < 0) {
		LOG_INF("Starting without mounted TF content (%d)", ret);
	}
	ret = papermono_organizer_init();
	if (ret < 0 && ret != -ENODEV) {
		LOG_WRN("Organizer content unavailable (%d)", ret);
	}
	ret = papermono_info_init();
	if (ret < 0) {
		LOG_WRN("Information cache unavailable (%d)", ret);
	}
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	initial = papermono_settings_get()->active_app;
#endif
	ret = papermono_app_router_init(&app_router, apps, ARRAY_SIZE(apps),
		initial);
	if (ret < 0) {
		return ret;
	}
	ret = papermono_display_scheduler_init(display, display_input_guard, NULL);
	if (ret < 0) {
		return ret;
	}
	/* Restore the saved frontlight: the driver powers the panel up dark. */
	frontlight_display = display;
	if (apply_frontlight() < 0) {
		LOG_WRN("Unable to restore frontlight level");
	}
	last_activity_ms = k_uptime_get();
	last_refresh_ms = last_activity_ms;
	return render_current(PAPERMONO_REFRESH_FULL);
}

int papermono_ui_demo_update(uint32_t update, uint32_t total)
{
	if (clock_label == NULL || footer_label == NULL) {
		return -EFAULT;
	}
	lv_label_set_text_fmt(clock_label, "10:%02u", (28U + update) % 60U);
	lv_label_set_text_fmt(footer_label, "PARTIAL %02u / %02u", update,
		total);
	return papermono_display_refresh(PAPERMONO_REFRESH_FAST);
}

int papermono_ui_run(void)
{
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	for (;;) {
		struct papermono_app_event event;
		const struct papermono_preferences *prefs = papermono_settings_get();
		const int64_t now = k_uptime_get();
		int64_t deadline = INT64_MAX;
		int ret;

#if CONFIG_PAPERMONO_UI_AUTO_REFRESH_MINUTES > 0
		deadline = last_refresh_ms +
			(int64_t)CONFIG_PAPERMONO_UI_AUTO_REFRESH_MINUTES * 60000;
#endif
		if (prefs->idle_sleep_minutes > 0U) {
			deadline = MIN(deadline, last_activity_ms +
				(int64_t)prefs->idle_sleep_minutes * 60000);
		}
		if (deadline == INT64_MAX) {
			ret = k_msgq_get(&ui_events, &event, K_FOREVER);
		} else {
			const int64_t remaining = MAX(deadline - now, 0);
			ret = k_msgq_get(&ui_events, &event,
				K_MSEC((int32_t)MIN(remaining, INT32_MAX)));
		}
		if (ret == 0) {
			if (event.type == PAPERMONO_APP_EVENT_CONTENT_CHANGED) {
				(void)atomic_cas(&pending_content_refresh,
					UI_SYSTEM_EVENT_QUEUED, 0);
			} else if (event.type == PAPERMONO_APP_EVENT_REFRESH) {
				(void)atomic_cas(&pending_background_refresh,
					UI_SYSTEM_EVENT_QUEUED, 0);
			}
			if (event.type != PAPERMONO_APP_EVENT_REFRESH &&
			    event.type != PAPERMONO_APP_EVENT_CONTENT_CHANGED) {
				last_activity_ms = k_uptime_get();
			}
			if (papermono_app_event_is_touch(&event)) {
				LOG_INF("UI processing touch type=%d x=%d y=%d duration=%u app=%d",
					event.type, event.x, event.y, event.duration_ms,
					app_router.current);
			}
			ret = process_event(&event);
		} else if (ret == -EAGAIN) {
			const int64_t current = k_uptime_get();

			if (prefs->idle_sleep_minutes > 0U &&
			    current - last_activity_ms >=
				(int64_t)prefs->idle_sleep_minutes * 60000) {
				if (automatic_sleep_inhibited()) {
					LOG_INF("Automatic sleep deferred while content transfer is active");
					last_activity_ms = current;
					ret = 0;
				} else {
					ret = enter_power_state(false);
				}
			} else {
				ret = render_current(PAPERMONO_REFRESH_AUTO);
			}
		}
		if (ret < 0) {
			papermono_status_led_report_runtime_fault(true);
			LOG_ERR("UI action failed (%d); keeping the event loop alive", ret);
			k_msleep(100);
			ret = render_current(PAPERMONO_REFRESH_FULL);
			if (ret < 0) {
				LOG_ERR("UI recovery refresh failed (%d); waiting for the next event",
					ret);
			} else {
				papermono_status_led_report_runtime_fault(false);
			}
		}
	}
#else
	return -ENOTSUP;
#endif
}

void papermono_ui_request_background_refresh(void)
{
	papermono_status_led_request_refresh();
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	(void)atomic_cas(&pending_background_refresh, 0,
		UI_SYSTEM_EVENT_PENDING);
	if (atomic_get(&input_blocked) == 0) {
		queue_pending_system_events();
	}
#endif
}

void papermono_ui_request_content_refresh(void)
{
	papermono_status_led_request_refresh();
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	(void)atomic_cas(&pending_content_refresh, 0,
		UI_SYSTEM_EVENT_PENDING);
	if (atomic_get(&input_blocked) == 0) {
		queue_pending_system_events();
	}
#endif
}
