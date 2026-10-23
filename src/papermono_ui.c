/* SPDX-License-Identifier: GPL-3.0-only */

/* THESIS: Extend the approved Field Notes paper interface to every device page.
 * OWN-WORLD: White I1 paper, black serif 48px titles and 24px primary text,
 * 40px empty-state titles, 16px metadata, ruled lists, original outline icons and square action buttons.
 * STORY: Read local content, see actual device state, and operate familiar
 * controls with explicit confirmation and a consistent way back.
 * FIRST VIEWPORT: 22px side margins; brand/date at 18px, title at 58px,
 * rule at 116px, content from 148px, return footer from 724px. Reader keeps
 * its approved quiet chrome. Visible geometry also drives hit testing.
 * FORM: Approved reference series field-notes-expanded/field-notes-remaining;
 * fixed 480x800 portrait Zephyr/LVGL, no gray, motion or simulated live data.
 * FINISH: unreviewed and undocumented is unfinished; this build ends with the
 * finish review, the verdict, DESIGN.md, and every shipping raster carrying
 * its provenance.
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

#include <papermono/almanac.h>
#include <papermono/app.h>
#if defined(CONFIG_PAPERMONO_BLE_MIC)
#include <papermono/ble_mic.h>
#endif
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
#include <papermono/quotes.h>
#include <papermono/reader.h>
#include <papermono/settings.h>
#include <papermono/status_led.h>
#include <papermono/sync.h>
#include <papermono/touch_gesture.h>
#include <papermono/touchpad.h>
#include <papermono/ui.h>
#include <papermono/ui_layout.h>
#include <papermono/weread.h>
#include <papermono/wifi_input.h>

LOG_MODULE_REGISTER(papermono_ui, LOG_LEVEL_INF);

LV_FONT_DECLARE(papermono_cjk_16);
LV_FONT_DECLARE(papermono_clock_148);
LV_FONT_DECLARE(papermono_reading_24);
LV_FONT_DECLARE(papermono_heading_48);
LV_FONT_DECLARE(papermono_state_40);
LV_IMAGE_DECLARE(papermono_icon_library);
LV_IMAGE_DECLARE(papermono_icon_weread);
LV_IMAGE_DECLARE(papermono_icon_calendar);
LV_IMAGE_DECLARE(papermono_icon_tasks);
LV_IMAGE_DECLARE(papermono_icon_notes);
LV_IMAGE_DECLARE(papermono_icon_remote);
LV_IMAGE_DECLARE(papermono_icon_content);
LV_IMAGE_DECLARE(papermono_icon_info);
LV_IMAGE_DECLARE(papermono_icon_tools);
LV_IMAGE_DECLARE(papermono_icon_settings);
LV_IMAGE_DECLARE(papermono_icon_power);
LV_IMAGE_DECLARE(papermono_icon_sleep);
LV_IMAGE_DECLARE(papermono_icon_bookmark);
LV_IMAGE_DECLARE(papermono_icon_document);
LV_IMAGE_DECLARE(papermono_icon_wifi);
LV_IMAGE_DECLARE(papermono_icon_bluetooth);
LV_IMAGE_DECLARE(papermono_icon_restore);
LV_IMAGE_DECLARE(papermono_icon_book_small);
LV_IMAGE_DECLARE(papermono_icon_headphones_small);
LV_IMAGE_DECLARE(papermono_icon_bookmark_small);
LV_IMAGE_DECLARE(papermono_icon_settings_small);
LV_IMAGE_DECLARE(papermono_icon_sun_small);
LV_IMAGE_DECLARE(papermono_icon_toc_small);
LV_IMAGE_DECLARE(papermono_icon_return_small);
LV_IMAGE_DECLARE(papermono_icon_library_small);
LV_IMAGE_DECLARE(papermono_icon_document_small);
LV_IMAGE_DECLARE(papermono_icon_info_small);
LV_IMAGE_DECLARE(papermono_icon_library_focal);
LV_IMAGE_DECLARE(papermono_icon_weread_focal);
LV_IMAGE_DECLARE(papermono_icon_calendar_focal);
LV_IMAGE_DECLARE(papermono_icon_tasks_focal);
LV_IMAGE_DECLARE(papermono_icon_document_focal);
LV_IMAGE_DECLARE(papermono_icon_bookmark_focal);
LV_IMAGE_DECLARE(papermono_icon_wifi_focal);

#define UI_WIDTH PAPERMONO_UI_WIDTH
#define UI_HEIGHT PAPERMONO_UI_HEIGHT
#define UI_HEADER_HEIGHT PAPERMONO_UI_HEADER_HEIGHT
#define UI_FOOTER_TOP PAPERMONO_UI_FOOTER_TOP
#define UI_TOUCH_ACTION_COOLDOWN_MS 40U
#define UI_CARD_RADIUS 0
/* LV_LABEL_LONG_DOT reserves three dot cells before it shortens a string, so a
 * lane sized to the bare advance still loses characters on a 1-bit panel.
 */
#define UI_DOT_RESERVE 24

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
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	REMOTE_MODE_VOICE,
#endif
};

struct remote_ui_state {
	enum remote_mode mode;
	int last_error;
};

static struct remote_ui_state remote_state;
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI) && defined(CONFIG_PAPERMONO_BLE_HID)
/* Bit zero publishes availability; each transition advances the gesture epoch. */
static atomic_t remote_touchpad_epoch;
static atomic_t remote_touchpad_activity;
static uint32_t touchpad_contact_epoch;
static bool touchpad_captured;
static struct papermono_touchpad touchpad_contact;
#endif
#if defined(CONFIG_PAPERMONO_BLE_MIC)
/* Bit zero publishes hold-to-talk availability for the voice page; every
 * transition advances the epoch, exactly like remote_touchpad_epoch, so a
 * contact captured on one page can never finish on another.
 */
static atomic_t remote_voice_epoch;
/* Set by the input callback when it starts a hold, cleared by that callback on
 * release or by the UI when the page, mode or power state owning the hold is
 * gone. Atomic so both threads can retire one hold exactly once.
 */
static atomic_t remote_voice_contact;
/* Last rejected request_start(), shown by the next voice page render. */
static atomic_t remote_voice_start_error;
#endif

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

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_LARGE_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_LARGE_BSS
#endif

#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
enum wifi_ui_mode { WIFI_UI_LIST, WIFI_UI_PASSWORD, WIFI_UI_FORGET };
static struct {
	enum wifi_ui_mode mode;
	struct papermono_wifi_state shown;
	struct papermono_wifi_network selected;
	struct papermono_wifi_input input;
	char display_text[64];
	size_t first;
	int last_error;
} wifi_ui PAPERMONO_LARGE_BSS;
/* Read from the input callback as well as the UI thread. Never log keyboard
 * coordinates: a sequence of key positions reveals the password. */
static atomic_t wifi_keyboard_active;
static atomic_t wifi_keyboard_overflow;
static atomic_t wifi_input_generation;
static atomic_t wifi_layout_transition;
static void wifi_advance_input(void)
{
	/* Do not publish a clickable layout before it exists on the panel. */
	atomic_set(&wifi_layout_transition, 1);
	atomic_inc(&wifi_input_generation);
}
#endif

#define LIBRARY_MAX_BOOKS 32U
#define LIBRARY_VISIBLE_BOOKS 5U

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
static bool library_resume_requested;
static struct papermono_note note_view PAPERMONO_LARGE_BSS;

enum confirmation_kind {
	CONFIRM_NONE,
	CONFIRM_SLEEP,
	CONFIRM_SHUTDOWN,
	CONFIRM_RESET_WIFI,
	CONFIRM_RESET_BLE,
	CONFIRM_RESET_SETTINGS,
	CONFIRM_FORMAT_TF,
};

static struct papermono_app_router app_router;
static lv_obj_t *clock_label;
static lv_obj_t *footer_label;
static enum confirmation_kind confirmation;
static enum papermono_app_id confirmation_origin = PAPERMONO_APP_POWER;
static int power_last_error;
static int frontlight_last_error;
/* Kept apart from frontlight_last_error: a failed factory reset is not a
 * backlight fault, and reporting it as one tells the user the wrong thing and
 * leaves the frontlight status stuck at "failed" for the rest of the session.
 */
static int settings_last_error;
/* Result of the last on-device TF format attempt, shown on the system page. */
static int storage_last_error;
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
/* Consecutive failed UI actions, used to stop a persistent fault from
 * triggering a full e-paper refresh on every event. Only the interactive event
 * loop reads it, so non-interactive profiles (HIL/demo) must not define it.
 */
static uint8_t ui_failure_streak;
#define UI_FAILURE_RECOVERY_ATTEMPTS 1U
#endif
static int64_t last_activity_ms;
static int64_t last_refresh_ms;

static void request_redraw(struct papermono_app_command *command);
static void read_snapshot(struct papermono_ui_snapshot *snapshot);
static void make_chevron(lv_obj_t *screen, int32_t x, int32_t y);

#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
static const struct device *const touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static const struct device *const buttons = DEVICE_DT_GET(DT_PATH(buttons));
static int32_t touch_x;
static int32_t touch_y;
static int32_t touch_start_x;
static int32_t touch_start_y;
static uint32_t touch_start_ms;
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
static uint32_t touch_input_generation;
#endif
static uint32_t last_touch_action_ms;
static bool touch_pressed;
static bool touch_navigation_sent;
#if defined(CONFIG_PAPERMONO_BLE_MIC)
/* Input-thread view of the current contact: this press started hold-to-talk,
 * so its release must stop the stream and skip gesture classification.
 */
static bool voice_hold;
#endif
static atomic_t input_blocked;
/* IRQ_STATUS3 can latch the wake click after the PMIC init has cleared it.
 * Accept only clicks captured after the fully started UI owns the event loop.
 */
static atomic_t power_input_ready;
#define UI_SYSTEM_EVENT_PENDING 1
#define UI_SYSTEM_EVENT_QUEUED 2
static atomic_t pending_background_refresh;
static atomic_t pending_content_refresh;
static struct k_spinlock deferred_touch_lock;
static struct papermono_app_event deferred_touch_event;
static bool deferred_touch_valid;
K_MSGQ_DEFINE(ui_events, sizeof(struct papermono_app_event), 12, 4);
#endif

#define UI_FIXED_LINE_CAPACITY 96U
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
	struct papermono_ui_snapshot snapshot;
	char date[48];
	lv_obj_t *label;
	read_snapshot(&snapshot);
	if (snapshot.rtc_valid) {
		static const char *const days[] = {"周日","周一","周二","周三","周四","周五","周六"};
		(void)snprintf(date, sizeof(date), "%02d月%02d日 %s",
			snapshot.time.tm_mon + 1, snapshot.time.tm_mday,
			days[CLAMP(snapshot.time.tm_wday, 0, 6)]);
	} else {
		(void)snprintf(date, sizeof(date), "时钟尚未校准");
	}
	label = make_label(screen, "PAPERMONO", &lv_font_montserrat_18, ink);
	lv_obj_set_pos(label, 22, 18);
	label = make_chinese_label(screen, date);
	lv_obj_set_pos(label, 242, 18);
	lv_obj_set_size(label, 216, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	label = make_label(screen, title, &papermono_heading_48, ink);
	lv_obj_set_pos(label, 22, 58);
	lv_obj_set_size(label, 436, 54);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	(void)make_ink_shape(screen, 22, 116, 436, 1, ink, 0);
}

static void make_footer(lv_obj_t *screen, const char *hint)
{
	(void)make_ink_shape(screen, 22, UI_DETAIL_FOOTER_TOP, 436, 1, ink, 0);
	(void)make_fixed_line(screen, 29, 750, 22, 757, 2, 0, 0, ink);
	(void)make_fixed_line(screen, 22, 757, 29, 764, 2, 0, 0, ink);
	lv_obj_t *label = make_label(screen, "返回", &papermono_reading_24, ink);
	lv_obj_set_pos(label, 42, 742);
	lv_obj_set_size(label, 80, 34);
	footer_label = make_chinese_label(screen, hint);
	lv_obj_set_pos(footer_label, 138, 738);
	lv_obj_set_size(footer_label, 320, 48);
	lv_label_set_long_mode(footer_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(footer_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}


/* Shared ruled detail components: only actual buttons receive a border. */
static lv_obj_t *detail_text(lv_obj_t *parent, const char *text,
	int32_t x, int32_t y, int32_t width, int32_t height, bool primary)
{
	lv_obj_t *label = make_label(parent, text,
		primary ? &papermono_reading_24 : &papermono_cjk_16, ink);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_size(label, width, height);
	lv_label_set_long_mode(label, height > 34 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
	return label;
}

static lv_obj_t *detail_row(lv_obj_t *screen, const struct papermono_rect *rect,
	const char *title, const char *meta, bool selected, bool arrow)
{
	lv_obj_t *row = lv_obj_create(screen);
	lv_obj_set_pos(row, rect->x, rect->y);
	lv_obj_set_size(row, rect->width, rect->height);
	set_box_style(row, paper, 0, 0);
	(void)make_ink_shape(row, 0, rect->height - 1, rect->width, 1, ink, 0);
	if (selected) {
		(void)make_ink_shape(row, 0, 10, 4, rect->height - 26, ink, 0);
	}
	(void)detail_text(row, title, 14, 10, rect->width - (arrow ? 52 : 28), 34, true);
	if (meta != NULL && meta[0] != '\0') {
		(void)detail_text(row, meta, 14, rect->height >= 96 ? 52 : 43,
			rect->width - 28, rect->height >= 96 ? 44 : 24, false);
	}
	if (arrow) {
		make_chevron(row, rect->width - 18, 20);
	}
	return row;
}

static void detail_button(lv_obj_t *screen, const struct papermono_rect *rect,
	const char *text, bool primary)
{
	lv_obj_t *button = make_outline_shape(screen, rect->x, rect->y,
		rect->width, rect->height, primary ? 2 : 1, 0);
	lv_obj_t *label = make_label(button, text, &papermono_reading_24, ink);
	lv_obj_set_width(label, rect->width - 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	lv_obj_center(label);
}

static void detail_image_at(lv_obj_t *parent, const lv_image_dsc_t *icon, int32_t x, int32_t y)
{
	lv_obj_t *image = lv_image_create(parent);
	lv_image_set_src(image, icon);
	lv_obj_set_style_image_recolor(image, ink, LV_PART_MAIN);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_pos(image, x, y);
}

static void detail_icon(lv_obj_t *screen, const lv_image_dsc_t *icon, int32_t y)
{
	detail_image_at(screen, icon, (UI_WIDTH - icon->header.w) / 2, y);
}

static void detail_confirmation(lv_obj_t *screen, const lv_image_dsc_t *icon,
	const char *title, const char *body, const char *action, const char *hint)
{
	detail_icon(screen, icon, 216);
	lv_obj_t *label = make_label(screen, title, &papermono_heading_48, ink);
	lv_obj_set_pos(label, 22, 352);
	lv_obj_set_size(label, 436, 58);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	label = detail_text(screen, body, 22, 434, 436, 70, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	detail_button(screen, &detail_confirm_rect, action, true);
	detail_button(screen, &detail_cancel_rect, "取消", false);
	make_footer(screen, hint);
}

static void detail_state_title(lv_obj_t *screen, const char *title, int32_t y)
{
	lv_obj_t *label = make_label(screen, title, &papermono_state_40, ink);
	lv_obj_set_pos(label, 22, y);
	lv_obj_set_size(label, 436, 50);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void detail_empty(lv_obj_t *screen, const lv_image_dsc_t *icon,
	const char *title, const char *body)
{
	detail_icon(screen, icon, 180);
	detail_state_title(screen, title, 384);
	lv_obj_t *label = detail_text(screen, body, 32, 450, 416, 64, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static lv_obj_t *detail_icon_row(lv_obj_t *screen, const struct papermono_rect *rect,
	const lv_image_dsc_t *icon, const char *title, const char *meta, bool selected, bool arrow)
{
	lv_obj_t *row = detail_row(screen, rect, "", NULL, selected, arrow);
	detail_image_at(row, icon, 14, 12);
	(void)detail_text(row, title, 76, 10, rect->width - (arrow ? 114 : 90), 34, true);
	if (meta && meta[0]) (void)detail_text(row, meta, 76, rect->height >= 96 ? 52 : 43,
		rect->width - 90, rect->height >= 96 ? 44 : 24, false);
	return row;
}

static void detail_power_option(lv_obj_t *screen, const struct papermono_rect *rect,
	const lv_image_dsc_t *icon, const char *title, const char *body)
{
	lv_obj_t *row = detail_row(screen, rect, "", NULL, false, true);
	detail_image_at(row, icon, 8, 26);
	(void)detail_text(row, title, 128, 28, 276, 38, true);
	(void)detail_text(row, body, 128, 82, 294, 66, false);
}

static void detail_number(lv_obj_t *screen, const char *value, int32_t x, int32_t y, int32_t width)
{
	lv_obj_t *label = make_label(screen, value, &papermono_heading_48, ink);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_size(label, width, 58);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static void detail_section(lv_obj_t *screen, int32_t y, const char *title)
{
	(void)detail_text(screen, title, 22, y, 436, 30, true);
}

static void detail_value(lv_obj_t *screen, int32_t y, const char *key, const char *value)
{
	(void)detail_text(screen, key, 22, y, 200, 30, false);
	lv_obj_t *label = detail_text(screen, value, 226, y, 232, 30, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
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





static void make_navigation(lv_obj_t *screen, enum papermono_app_id active)
{
	static const char *const titles[] = { "首页", "应用", "设置" };

	(void)make_ink_shape(screen, 0, UI_NAV_TOP, UI_WIDTH, 1, ink, 0);
	for (size_t index = 0; index < ARRAY_SIZE(navigation_regions); ++index) {
		const struct papermono_rect *rect = &navigation_regions[index].rect;
		lv_obj_t *label = make_label(screen, titles[index], &papermono_reading_24, ink);

		lv_obj_set_pos(label, rect->x, rect->y + 20);
		lv_obj_set_size(label, rect->width, 34);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
		if (navigation_regions[index].action == active) {
			(void)make_ink_shape(screen, rect->x + 56, rect->y + 60,
				48, 4, ink, 0);
		}
	}
}

static void make_chevron(lv_obj_t *screen, int32_t x, int32_t y)
{
	(void)make_fixed_line(screen, x, y, x + 7, y + 7, 2, 0, 0, ink);
	(void)make_fixed_line(screen, x + 7, y + 7, x, y + 14, 2, 0, 0, ink);
}

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
	const struct papermono_rect *reader = &home_regions[HOME_REGION_READER].rect;
	const struct papermono_rect *calendar = &home_regions[HOME_REGION_CALENDAR].rect;
	const struct papermono_rect *tasks = &home_regions[HOME_REGION_TASKS].rect;
	lv_obj_t *label;
	char status[48];
	char battery[16];
	const int recent_ret = papermono_reader_recent(&recent_book);
	const bool progress_valid = recent_ret == 0 && library_state.reader.page_loaded &&
		strcmp(library_state.reader.book.id, recent_book.id) == 0;
	const uint8_t progress = progress_valid ? library_state.page.approximate_percent : 0;

	ARG_UNUSED(context);
	read_snapshot(&snapshot);
	format_connectivity_status(status, sizeof(status));
	format_voltage(battery, sizeof(battery), snapshot.battery_valid, snapshot.battery_mv);
	label = make_label(screen, "PAPERMONO", &lv_font_montserrat_18, ink);
	lv_obj_set_pos(label, 22, 14);
	lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
	label = make_chinese_label(screen, status);
	lv_obj_set_pos(label, 184, 12);
	lv_obj_set_size(label, 274, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	label = make_chinese_label(screen, battery);
	lv_obj_set_pos(label, 352, 38);
	lv_obj_set_size(label, 106, 22);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

	label = make_chinese_label(screen, "时钟尚未校准");
	lv_obj_set_pos(label, 22, 66);
	clock_label = make_label(screen, snapshot.rtc_valid ? "00:00" : "TIME",
		snapshot.rtc_valid ? &papermono_clock_148 : &lv_font_montserrat_48, ink);
	lv_obj_set_pos(clock_label, 18, snapshot.rtc_valid ? 94 : 120);
	lv_obj_set_size(clock_label, 440, snapshot.rtc_valid ? 122 : 64);
	if (snapshot.rtc_valid) {
		static const char *const weekdays[] = {
			"周日", "周一", "周二", "周三", "周四", "周五", "周六",
		};
		const struct papermono_preferences *prefs = papermono_settings_get();
		int hour = snapshot.time.tm_hour;
		const char *period = "";
		if (!prefs->use_24_hour) {
			period = hour >= 12 ? " PM" : " AM";
			hour %= 12;
			hour = hour == 0 ? 12 : hour;
		}
		lv_label_set_text_fmt(clock_label, "%02d:%02d", hour, snapshot.time.tm_min);
		lv_label_set_text_fmt(label, "%02d月%02d日  %s%s",
			snapshot.time.tm_mon + 1, snapshot.time.tm_mday,
			snapshot.time.tm_wday >= 0 && snapshot.time.tm_wday < 7 ?
			weekdays[snapshot.time.tm_wday] : "", period);
	}

	(void)make_ink_shape(screen, reader->x, reader->y, reader->width, 1, ink, 0);
	label = make_label(screen, recent_ret == 0 ? "继续阅读" : "打开书库", &papermono_reading_24, ink);
	lv_obj_set_pos(label, reader->x, reader->y + 18);
	make_chevron(screen, reader->x + reader->width - 12, reader->y + 20);
	/* An outlined text spine, not a fabricated book-cover image. */
	(void)make_outline_shape(screen, reader->x, reader->y + 58, 88, 136, 2, 0);
	(void)make_ink_shape(screen, reader->x + 10, reader->y + 58, 2, 136, ink, 0);
	label = make_chinese_label(screen, recent_ret == 0 ?
		(recent_book.kind == PAPERMONO_CONTENT_EPUB ? "EPUB" : "TXT") : "书库");
	lv_obj_set_pos(label, reader->x + 20, reader->y + 112);
	lv_obj_set_size(label, 60, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	label = make_label(screen, recent_ret == 0 ? recent_book.title : "还没有最近阅读", &papermono_reading_24, ink);
	lv_obj_set_pos(label, reader->x + 112, reader->y + 66);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_size(label, reader->width - 112, 60);
	label = make_chinese_label(screen, recent_ret == 0 ?
		"轻触继续阅读" : "从内容中心导入书籍");
	lv_obj_set_pos(label, reader->x + 112, reader->y + 132);
	lv_obj_set_size(label, reader->width - 112, 24);
	if (progress_valid) {
		(void)make_ink_shape(screen, reader->x + 112, reader->y + 176, 244, 1, ink, 0);
		if (progress > 0) {
			(void)make_ink_shape(screen, reader->x + 112, reader->y + 174,
				MAX(1, 244 * MIN(progress, 100) / 100), 4, ink, 0);
		}
		label = make_chinese_label(screen, "");
		lv_label_set_text_fmt(label, "%u%%", progress);
	} else {
		label = make_chinese_label(screen, recent_ret == 0 ? "已有记录" : "未开始");
	}
	lv_obj_set_pos(label, reader->x + 364, reader->y + 166);
	lv_obj_set_size(label, 72, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

	const struct papermono_calendar_time now = {
		.year = snapshot.time.tm_year + 1900, .month = snapshot.time.tm_mon + 1,
		.day = snapshot.time.tm_mday, .hour = snapshot.time.tm_hour,
		.minute = snapshot.time.tm_min, .second = snapshot.time.tm_sec,
	};
	const bool have_event = snapshot.rtc_valid &&
		papermono_organizer_get_next_event(&now, &next_event) == 0;
	(void)make_ink_shape(screen, calendar->x, calendar->y, calendar->width, 1, ink, 0);
	label = make_label(screen, "下一日程", &papermono_reading_24, ink);
	lv_obj_set_pos(label, calendar->x, calendar->y + 18);
	make_chevron(screen, calendar->x + calendar->width - 12, calendar->y + 20);
	if (have_event) {
		(void)make_ink_shape(screen, calendar->x + 6, calendar->y + 60, 3, 34, ink, 0);
		label = make_chinese_label(screen, "");
		lv_label_set_text_fmt(label, "%02u:%02u", next_event.start.hour, next_event.start.minute);
		lv_obj_set_pos(label, calendar->x + 24, calendar->y + 66);
		label = make_chinese_label(screen, next_event.title);
		lv_obj_set_pos(label, calendar->x + 128, calendar->y + 66);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, calendar->width - 128, 24);
		/* Keep tomorrow's event distinguishable from today's. */
		if (next_event.start.year != now.year || next_event.start.month != now.month ||
		    next_event.start.day != now.day) {
			label = make_chinese_label(screen, "");
			lv_label_set_text_fmt(label, "%04d/%02u/%02u", next_event.start.year,
				next_event.start.month, next_event.start.day);
			lv_obj_set_pos(label, calendar->x + 128, calendar->y + 92);
		}
	} else {
		label = make_chinese_label(screen, snapshot.rtc_valid ? "暂无日程" : "校时后显示下一日程");
		lv_obj_set_pos(label, calendar->x + 4, calendar->y + 66);
	}

	(void)make_ink_shape(screen, tasks->x, tasks->y, tasks->width, 1, ink, 0);
	label = make_label(screen, "待办", &papermono_reading_24, ink);
	lv_obj_set_pos(label, tasks->x, tasks->y + 18);
	make_chevron(screen, tasks->x + tasks->width - 12, tasks->y + 20);
	size_t visible_tasks = 0;
	for (size_t index = 0; index < papermono_organizer_task_count() && visible_tasks < 2; ++index) {
		struct papermono_task task;
		if (papermono_organizer_get_task(index, &task) < 0 || task.done) {
			continue;
		}
		const int32_t y = tasks->y + 60 + visible_tasks * 40;
		make_checkbox(screen, tasks->x + 4, y, false);
		label = make_label(screen, task.title, &papermono_reading_24, ink);
		lv_obj_set_pos(label, tasks->x + 40, y);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_size(label, tasks->width - 40, 34);
		++visible_tasks;
	}
	if (visible_tasks == 0) {
		label = make_chinese_label(screen, papermono_organizer_task_count() > 0 ?
			"所有待办已完成" : "没有未完成待办");
		lv_obj_set_pos(label, tasks->x + 4, tasks->y + 60);
	}
	make_navigation(screen, PAPERMONO_APP_HOME);
	return 0;
}

static void render_launcher_item(lv_obj_t *screen,
	const struct launcher_layout_item *item,
	const struct papermono_rect *rect,
	const struct papermono_ui_snapshot *snapshot)
{
	static const lv_image_dsc_t *const icons[] = {
		&papermono_icon_library, &papermono_icon_weread, &papermono_icon_calendar,
		&papermono_icon_tasks, &papermono_icon_notes, &papermono_icon_remote,
		&papermono_icon_content, &papermono_icon_info, &papermono_icon_tools,
		&papermono_icon_settings, &papermono_icon_power,
	};
	/* icons[] is indexed by enum launcher_motif; an entry added without its
	 * icon would be an out-of-bounds read at draw time.
	 */
	_Static_assert(sizeof(icons) / sizeof(icons[0]) == LAUNCHER_MOTIF_POWER + 1U,
		"icons must have one entry per launcher motif");
	const struct papermono_app_descriptor *app = find_app_descriptor(item->id);
	if (app == NULL) {
		return;
	}
	(void)make_outline_shape(screen, rect->x, rect->y, rect->width, rect->height, 1, 0);
	lv_obj_t *image = lv_image_create(screen);
	lv_image_set_src(image, icons[item->motif]);
	lv_obj_set_style_image_recolor(image, ink, LV_PART_MAIN);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_pos(image, rect->x + (rect->width - 96) / 2, rect->y + 22);
	if (item->motif == LAUNCHER_MOTIF_CALENDAR) {
		lv_obj_t *day = make_chinese_label(screen, "--");
		if (snapshot->rtc_valid) {
			lv_label_set_text_fmt(day, "%02d", snapshot->time.tm_mday);
		}
		lv_obj_set_pos(day, rect->x, rect->y + 72);
		lv_obj_set_size(day, rect->width, 34);
		lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	}
	/* The cell uses the same title the application registers, so the index
	 * and the page it opens never disagree.
	 */
	lv_obj_t *label = make_label(screen, app->title, &papermono_reading_24, ink);
	lv_obj_set_pos(label, rect->x + 4, rect->y + 138);
	lv_obj_set_size(label, rect->width - 8, 34);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static int render_launcher(void *context, lv_obj_t *screen)
{
	struct papermono_ui_snapshot snapshot;
	struct papermono_rect rect;
	const size_t page_count = (ARRAY_SIZE(launcher_items) + LAUNCHER_PAGE_SIZE - 1) / LAUNCHER_PAGE_SIZE;
	const size_t first_item = launcher_page * LAUNCHER_PAGE_SIZE;

	ARG_UNUSED(context);
	read_snapshot(&snapshot);
	lv_obj_t *label = make_label(screen, "应用", &papermono_heading_48, ink);
	lv_obj_set_pos(label, 22, 36);
	label = make_chinese_label(screen, "");
	lv_label_set_text_fmt(label, "%02u / %02u", (unsigned int)(launcher_page + 1), (unsigned int)page_count);
	lv_obj_set_pos(label, 346, 48);
	lv_obj_set_size(label, 112, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	(void)make_ink_shape(screen, 22, 94, 436, 1, ink, 0);
	for (size_t slot = 0; slot < LAUNCHER_PAGE_SIZE; ++slot) {
		const size_t index = first_item + slot;
		if (index < ARRAY_SIZE(launcher_items) &&
		    papermono_interaction_grid_cell_rect(&launcher_grid, slot, &rect) == 0) {
			render_launcher_item(screen, &launcher_items[index], &rect, &snapshot);
		}
	}
	make_navigation(screen, PAPERMONO_APP_LAUNCHER);
	return 0;
}

static int render_content_center(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	make_header(screen, "内容中心");
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	struct papermono_content_server_state state;
	char text[192];
	papermono_content_server_get_state(&state);
	(void)detail_text(screen, "在浏览器中管理内容", 22, 132, 436, 24, false);
	detail_section(screen, 168, "首次配网");
	(void)snprintf(text, sizeof(text), "网络：%s\n密码：%s",
		state.ap_enabled ? state.ap_ssid : "接入点未启用",
		state.ap_enabled ? state.ap_password : "--");
	(void)detail_text(screen, text, 22, 208, 436, 52, false);
	(void)detail_text(screen, state.ap_enabled ? "192.168.4.1" : "接入点已关闭",
		22, 266, 436, 38, true);
	(void)detail_text(screen, state.ap_verified ? "接口自检通过" :
		state.ap_enabled ? "正在验证接入点" : "", 22, 308, 436, 24, false);
	(void)make_ink_shape(screen, 22, 346, 436, 1, ink, 0);
	detail_section(screen, 362, "浏览器会话");
	if (state.client_paired) {
		(void)detail_text(screen, "本地会话已建立", 22, 406, 436, 34, true);
		(void)detail_text(screen, "浏览器刷新会自动恢复", 22, 446, 436, 24, false);
	} else {
		(void)detail_text(screen, "打开设备地址，输入一次性配对码", 22, 402, 436, 24, false);
		detail_number(screen, state.pairing_code, 22, 430, 436);
	}
	(void)make_ink_shape(screen, 22, 498, 436, 1, ink, 0);
	detail_section(screen, 512, "局域网");
	(void)snprintf(text, sizeof(text), "%s\n内容版本：%u",
		state.station_connected ? (state.station_ipv4[0] ? state.station_ipv4 : "等待 DHCP 地址") :
		state.station_connecting ? "正在连接" : "未连接", state.content_generation);
	(void)detail_text(screen, text, 22, 552, 436, 68, state.station_connected);
	(void)make_ink_shape(screen, 22, 630, 436, 1, ink, 0);
	detail_section(screen, 642, "管理范围");
	(void)detail_text(screen, "书籍 · 日历 · 待办 · 便签", 22, 680, 436, 24, false);
#else
	detail_empty(screen, &papermono_icon_content, "内容中心未启用", "当前固件未包含本地 HTTP 服务。");
#endif
	make_footer(screen, "浏览器传输在后台执行");
	return 0;
}

#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
/* Wi-Fi extends Field Notes: a ruled four-network list, then a dedicated
 * bounded ASCII keyboard. Real state only; no LVGL calls from Wi-Fi workers. */
static void wifi_input_close(void)
{
	wifi_advance_input();
	atomic_set(&wifi_keyboard_active, 0);
	papermono_wifi_input_wipe(&wifi_ui.input);
	volatile char *wipe = wifi_ui.display_text;
	for (size_t i = 0; i < sizeof(wifi_ui.display_text); ++i) wipe[i] = 0;
	wifi_ui.mode = WIFI_UI_LIST;
}

static const char *wifi_error_text(int error)
{
	switch (error) {
	case 0: return "";
	case -EBUSY: return "网络正在处理，请稍后重试";
	case -ENOTSUP: return "此网络不支持，请选择 2.4 GHz WPA2 网络";
	case -EINVAL: return "密码需为 8 至 63 位字母、数字或符号";
	case -ENOSPC: return "密码最多 63 位，请退格修改";
	case -EOVERFLOW: return "输入过快，请返回后重新输入密码";
	case -ETIMEDOUT: return "连接超时，请检查密码并重试";
	default: return "操作失败，请检查密码和网络后重试";
	}
}

static int wifi_open(void *context)
{
	ARG_UNUSED(context);
	wifi_input_close();
	wifi_ui.first = 0;
	wifi_ui.last_error = papermono_content_server_scan_wifi();
	if (wifi_ui.last_error == -EBUSY) wifi_ui.last_error = 0;
	return 0;
}

static void wifi_close(void *context)
{
	ARG_UNUSED(context);
	wifi_input_close();
}

static int render_wifi(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	char text[160];
	make_header(screen, "连接设置");
	if (wifi_ui.mode == WIFI_UI_FORGET) {
		detail_confirmation(screen, &papermono_icon_wifi, "确认忘记网络？",
			"将断开连接并清除网络密码。\n设备接入点与配对码也会更新。\nTF 卡内容不会删除。",
			"确认忘记", "取消不会更改连接");
		return 0;
	}
	if (wifi_ui.mode == WIFI_UI_PASSWORD) {
		(void)detail_text(screen, wifi_ui.selected.ssid, 22, 132, 436, 34, true);
		volatile char *wipe = wifi_ui.display_text;
		for (size_t i = 0; i < sizeof(wifi_ui.display_text); ++i) wipe[i] = 0;
		(void)papermono_wifi_input_display(&wifi_ui.input,
			wifi_ui.display_text, sizeof(wifi_ui.display_text));
		const size_t length = strlen(wifi_ui.display_text);
		(void)snprintf(text, sizeof(text), "密码 · %zu / 63 位", length);
		(void)detail_text(screen, text, 22, 172, 436, 24, false);
		/* The complete value fits in three 16px-font lines (63 ASCII bytes),
		 * so showing a password permits checking every character, not a tail.
		 * Static WRAP text never allocates a second secret copy in LVGL. */
		lv_obj_t *password = make_label(screen, "", &papermono_cjk_16, ink);
		lv_obj_set_pos(password, 22, 198);
		lv_obj_set_size(password, 436, 56);
		lv_label_set_long_mode(password, LV_LABEL_LONG_WRAP);
		lv_label_set_text_static(password, wifi_ui.display_text);
		(void)detail_text(screen, wifi_ui.last_error ? wifi_error_text(wifi_ui.last_error) :
			"区分大小写 · 支持空格和符号", 22, 258, 436, 28, false);
		detail_button(screen, &wifi_connect_rect, "保存并连接", true);
		for (size_t i = 0; i < papermono_wifi_input_key_count(); ++i) {
			struct papermono_wifi_input_key key;
			(void)papermono_wifi_input_key_at(&wifi_ui.input, i, &key);
			struct papermono_rect rect = {key.x, key.y, key.width, key.height};
			lv_obj_t *button = make_outline_shape(screen, rect.x, rect.y,
				rect.width, rect.height, 1, 0);
			/* Two full-size CJK glyphs exceed a 44px character key. */
			lv_obj_t *label = make_label(button, key.label,
				key.character == ' ' ? &papermono_cjk_16 : &papermono_reading_24, ink);
			lv_obj_set_size(label, rect.width - 4, 30);
			lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
			lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
			lv_obj_center(label);
		}
		make_footer(screen, "返回取消输入 · 密码不写入日志");
		return 0;
	}
	const uint32_t previous_scan = wifi_ui.shown.scan_generation;
	papermono_content_server_get_wifi_state(&wifi_ui.shown);
	if (previous_scan != wifi_ui.shown.scan_generation) wifi_advance_input();
	struct papermono_content_server_state network;
	papermono_content_server_get_state(&network);
	if (wifi_ui.first >= wifi_ui.shown.count) wifi_ui.first = 0;
	(void)detail_text(screen, wifi_ui.shown.configured ? wifi_ui.shown.saved_ssid :
		"尚未保存网络", 22, 132, 436, 34, true);
	const char *status = wifi_ui.shown.request_busy ? "正在处理网络设置，请稍候" :
		wifi_ui.shown.connection == PAPERMONO_WIFI_CONNECTED ?
		(network.station_ipv4[0] ? network.station_ipv4 : "已连接，等待网络地址") :
		wifi_ui.shown.connection == PAPERMONO_WIFI_CONNECTING ? "正在连接已保存的网络" :
		(wifi_ui.shown.connection == PAPERMONO_WIFI_CONNECTION_ERROR || wifi_ui.shown.last_connection_error) ? "操作失败，请检查密码或重新选择网络" :
		"未连接 · 选择下方网络";
	(void)detail_text(screen, status, 22, 174, 436, 34, false);
	const bool idle = !wifi_ui.shown.request_busy && !wifi_ui.shown.scan_busy &&
		wifi_ui.shown.connection != PAPERMONO_WIFI_CONNECTING;
	if (idle && wifi_ui.shown.scan_error != -ETIMEDOUT)
		detail_button(screen, &wifi_scan_rect, "重新扫描", false);
	else (void)detail_text(screen, wifi_ui.shown.scan_busy ? "扫描中" : "请稍候", 28, 232, 130, 28, false);
	if (idle && wifi_ui.shown.configured) {
		detail_button(screen, &wifi_reconnect_rect, "重新连接", false);
		detail_button(screen, &wifi_forget_rect, "忘记网络", false);
	} else {
		(void)detail_text(screen, "重新连接", 180, 232, 128, 28, false);
		(void)detail_text(screen, "忘记网络", 328, 232, 128, 28, false);
	}
	(void)detail_text(screen, wifi_ui.last_error ? wifi_error_text(wifi_ui.last_error) :
		wifi_ui.shown.scan_error == -ETIMEDOUT ? "扫描超时，请重启设备后重试" :
		wifi_ui.shown.scan_error ? "扫描失败，请重新扫描" :
		wifi_ui.shown.scan_busy ? "正在扫描 2.4 GHz 网络" : "可用网络 · 轻触选择", 22, 276, 436, 24, false);
	for (size_t row = 0; row < 4; ++row) {
		const size_t index = wifi_ui.first + row;
		if (index >= wifi_ui.shown.count) break;
		struct papermono_wifi_network *item = &wifi_ui.shown.networks[index];
		struct papermono_rect rect;
		(void)papermono_interaction_grid_cell_rect(&wifi_network_grid, row, &rect);
		(void)snprintf(text, sizeof(text), "%s · %d dBm · 信道 %u",
			!item->supported ? "不支持" : item->security == PAPERMONO_WIFI_SECURITY_OPEN ? "开放网络" : "WPA2", item->rssi, item->channel);
		(void)detail_row(screen, &rect, item->ssid[0] ? item->ssid : "隐藏网络", text,
			wifi_ui.shown.configured && strcmp(item->ssid, wifi_ui.shown.saved_ssid) == 0, item->supported && idle);
	}
	if (!wifi_ui.shown.count && !wifi_ui.shown.scan_busy && !wifi_ui.shown.scan_error) {
		/* Keep saved-network controls above the empty result. The focal icon
		 * and state typography match other Field Notes empty pages. */
		detail_icon(screen, &papermono_icon_wifi_focal, 306);
		detail_state_title(screen, "未发现可用网络", 502);
		lv_obj_t *label = detail_text(screen,
			"请确认路由器已开启 2.4 GHz Wi-Fi，\n靠近设备后重新扫描。", 32, 560, 416, 66, false);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	}
	if (idle && wifi_ui.first >= 4) detail_button(screen, &wifi_previous_rect, "上一页", false);
	if (idle && wifi_ui.first + 4 < wifi_ui.shown.count) detail_button(screen, &wifi_next_rect, "下一页", false);
	(void)snprintf(text, sizeof(text), "%u 个网络 · %u / %u 页",
		wifi_ui.shown.count, (unsigned)(wifi_ui.first / 4 + 1),
		(unsigned)MAX(1, (wifi_ui.shown.count + 3) / 4));
	make_footer(screen, text);
	return 0;
}

static int wifi_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	ARG_UNUSED(context);
	const bool back = event->type == PAPERMONO_APP_EVENT_BACK ||
		(event->type == PAPERMONO_APP_EVENT_PREVIOUS && papermono_app_event_is_touch(event)) ||
		(event->type == PAPERMONO_APP_EVENT_TAP && event->y >= UI_DETAIL_FOOTER_TOP);
	if (back) {
		if (wifi_ui.mode != WIFI_UI_LIST) {
			wifi_input_close();
			wifi_ui.last_error = 0;
			request_redraw(command);
		} else command->type = PAPERMONO_APP_COMMAND_BACK;
		return 0;
	}
	if (wifi_ui.mode == WIFI_UI_FORGET) {
		if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &detail_cancel_rect)) {
			wifi_ui.mode = WIFI_UI_LIST;
			wifi_advance_input();
			request_redraw(command);
		} else if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &detail_confirm_rect)) {
			wifi_ui.last_error = papermono_content_server_forget_wifi();
			wifi_ui.mode = WIFI_UI_LIST;
			wifi_advance_input();
			request_redraw(command);
		}
		return 0;
	}
	if (wifi_ui.mode == WIFI_UI_PASSWORD) {
		if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_connect_rect)) {
			if (atomic_get(&wifi_keyboard_overflow)) {
				wifi_ui.last_error = -EOVERFLOW;
				request_redraw(command);
				return 0;
			}
			wifi_ui.last_error = papermono_content_server_connect_wifi(&wifi_ui.selected, wifi_ui.input.text);
			if (!wifi_ui.last_error) wifi_input_close();
			request_redraw(command);
		} else if (event->type == PAPERMONO_APP_EVENT_TAP) {
			enum papermono_wifi_input_layer old_layer = wifi_ui.input.layer;
			int ret = papermono_wifi_input_tap(&wifi_ui.input, event->x, event->y);
			if (old_layer != wifi_ui.input.layer) wifi_advance_input();
			if (ret != 0) {
				wifi_ui.last_error = ret < 0 ? ret : 0;
				request_redraw(command);
			}
		}
		return 0;
	}
	if (wifi_ui.shown.request_busy || wifi_ui.shown.scan_busy ||
	    wifi_ui.shown.connection == PAPERMONO_WIFI_CONNECTING) return 0;
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_scan_rect)) {
		if (wifi_ui.shown.scan_error == -ETIMEDOUT) return 0;
		wifi_ui.last_error = papermono_content_server_scan_wifi();
		wifi_ui.first = 0;
	} else if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_reconnect_rect)) {
		if (!wifi_ui.shown.configured) return 0;
		wifi_ui.last_error = papermono_content_server_connect_saved();
	} else if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_forget_rect)) {
		if (wifi_ui.shown.configured) {
			wifi_ui.mode = WIFI_UI_FORGET;
			wifi_advance_input();
		}
	} else if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_previous_rect) ||
		(event->type == PAPERMONO_APP_EVENT_PREVIOUS && papermono_app_event_is_button(event))) {
		if (wifi_ui.first >= 4) { wifi_ui.first -= 4; wifi_advance_input(); }
	} else if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP, &wifi_next_rect) || event->type == PAPERMONO_APP_EVENT_NEXT) {
		if (wifi_ui.first + 4 < wifi_ui.shown.count) { wifi_ui.first += 4; wifi_advance_input(); }
	} else {
		size_t row;
		if (papermono_interaction_spaced_grid_index(event, PAPERMONO_INTERACTION_TAP, &wifi_network_grid, &row) < 0 ||
		    wifi_ui.first + row >= wifi_ui.shown.count) return 0;
		wifi_ui.selected = wifi_ui.shown.networks[wifi_ui.first + row];
		if (!wifi_ui.selected.supported) wifi_ui.last_error = -ENOTSUP;
		else if (wifi_ui.selected.security == PAPERMONO_WIFI_SECURITY_OPEN) {
			wifi_ui.last_error = papermono_content_server_connect_wifi(&wifi_ui.selected, "");
		} else {
			(void)papermono_wifi_input_init(&wifi_ui.input, PAPERMONO_WIFI_INPUT_PASSWORD);
			wifi_ui.last_error = 0;
			wifi_ui.mode = WIFI_UI_PASSWORD;
			atomic_set(&wifi_keyboard_overflow, 0);
			wifi_advance_input();
			atomic_set(&wifi_keyboard_active, 1);
		}
	}
	request_redraw(command);
	return 0;
}
#endif

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
	.y = 186,
	.cell_width = 436,
	.cell_height = 98,
	.column_gap = 0,
	.row_gap = 6,
	.columns = 1,
	.rows = PAPERMONO_WEREAD_VISIBLE_BOOKS,
};

static int render_weread_empty(struct weread_ui_state *state, lv_obj_t *screen)
{
	make_header(screen, "微信读书");
	detail_icon(screen, &papermono_icon_weread_focal, 174);
	detail_state_title(screen, "本地书架不可用", 376);
	lv_obj_t *label = detail_text(screen, weread_error_text(state->last_error), 22, 440, 436, 24, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	detail_button(screen, &weread_import_rect, "打开内容中心", true);
	label = detail_text(screen, "上传 weread-v1.json\n类型选择「微信读书书架」", 22, 570, 436, 52, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	(void)make_ink_shape(screen, 22, 644, 436, 1, ink, 0);
	detail_section(screen, 662, "只读书架摘要");
	(void)detail_text(screen, "通过手机或电脑导入摘要信息", 22, 698, 436, 24, false);
	make_footer(screen, "长按重载");
	return 0;
}

static int render_weread_list(struct weread_ui_state *state,
	lv_obj_t *screen, const struct papermono_weread_status *status)
{
	char text[192];
	const size_t first = papermono_weread_view_first(&state->view);
	make_header(screen, "微信读书");
	(void)snprintf(text, sizeof(text), "本地书架 · %zu / %zu 本%s", status->count,
		status->total, status->truncated ? " · 已截断" : "");
	(void)detail_text(screen, text, 22, 128, 436, 24, false);
	(void)snprintf(text, sizeof(text), "同步时间：%s", status->synced_at[0] ? status->synced_at : "--");
	(void)detail_text(screen, text, 22, 154, 436, 24, false);
	for (size_t row = 0; row < PAPERMONO_WEREAD_VISIBLE_BOOKS; ++row) {
		struct papermono_weread_book book;
		struct papermono_rect rect;
		const size_t index = first + row;
		if (index >= status->count || papermono_weread_get(index, &book) < 0) break;
		(void)papermono_interaction_grid_cell_rect(&weread_book_grid, row, &rect);
		char progress[32];
		if (book.has_progress) (void)snprintf(progress, sizeof(progress), "%u%%", book.progress);
		else (void)snprintf(progress, sizeof(progress), "未提供进度");
		(void)snprintf(text, sizeof(text), "%s · %s · %s%s",
			book.author[0] ? book.author : "作者未知", weread_kind_text(book.kind),
			progress, book.finished ? " · 已读完" : "");
		lv_obj_t *item = detail_icon_row(screen, &rect, book.kind == PAPERMONO_WEREAD_AUDIO ?
			&papermono_icon_headphones_small : book.kind == PAPERMONO_WEREAD_ARTICLES ?
			&papermono_icon_document_small : &papermono_icon_book_small, book.title, NULL,
			index == state->view.selected, true);
		(void)detail_text(item, text, 76, 46, 346, 24, false);
		(void)snprintf(text, sizeof(text), "更新：%s", book.updated_at[0] ? book.updated_at : "--");
		(void)detail_text(item, text, 76, 72, 346, 24, false);
	}
	(void)snprintf(text, sizeof(text), "%02u / %02u · 轻触查看详情",
		(unsigned)(first / PAPERMONO_WEREAD_VISIBLE_BOOKS + 1),
		(unsigned)((status->count + PAPERMONO_WEREAD_VISIBLE_BOOKS - 1) / PAPERMONO_WEREAD_VISIBLE_BOOKS));
	make_footer(screen, text);
	return 0;
}

static int render_weread_detail(struct weread_ui_state *state, lv_obj_t *screen)
{
	struct papermono_weread_book book;
	char value[96];
	if (papermono_weread_get(state->view.selected, &book) < 0) {
		state->view.mode = PAPERMONO_WEREAD_VIEW_LIST;
		state->last_error = -ENOENT;
		return render_weread_empty(state, screen);
	}
	make_header(screen, "微信读书");
	(void)detail_text(screen, "书籍详情", 22, 132, 436, 24, false);
	(void)make_outline_shape(screen, 22, 188, 80, 116, 2, 0);
	(void)make_ink_shape(screen, 32, 188, 1, 116, ink, 0);
	(void)detail_text(screen, book.title, 124, 186, 334, 72, true);
	(void)detail_text(screen, book.author[0] ? book.author : "作者未知", 124, 270, 334, 24, false);
	if (book.has_progress) (void)snprintf(value, sizeof(value), "%u%%", book.progress);
	else (void)snprintf(value, sizeof(value), "未提供进度");
	if (book.has_progress) detail_number(screen, value, 124, 310, 334);
	else (void)detail_text(screen, value, 124, 310, 334, 34, true);
	(void)make_ink_shape(screen, 22, 370, 436, 1, ink, 0);
	if (book.has_progress && book.progress > 0) {
		(void)make_ink_shape(screen, 22, 367, 436 * MIN(book.progress, 100U) / 100, 4, ink, 0);
	}
	detail_value(screen, 402, "类型", weread_kind_text(book.kind));
	const uint32_t minutes = book.reading_seconds / 60U;
	(void)snprintf(value, sizeof(value), "%u 小时 %u 分钟", minutes / 60U, minutes % 60U);
	detail_value(screen, 446, "累计阅读", value);
	detail_value(screen, 490, "更新时间", book.updated_at[0] ? book.updated_at : "--");
	detail_value(screen, 534, "阅读状态", book.finished ? "已读完" : "未读完");
	(void)make_ink_shape(screen, 22, 594, 436, 1, ink, 0);
	detail_section(screen, 614, "本地书架摘要");
	(void)detail_text(screen, "正文请在手机或电脑端阅读。", 22, 658, 436, 28, false);
	make_footer(screen, "左右键切换书籍");
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
	const bool resume_reader = library_resume_requested ||
		(prefs->active_app == PAPERMONO_APP_LIBRARY && prefs->reader_resume);

	library_resume_requested = false;

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
					(void)papermono_settings_set_reader_resume(true);
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

static int render_library_list(struct library_ui_state *state, lv_obj_t *screen)
{
	char text[128];
	make_header(screen, "书库");
	if (state->book_count == 0U) {
		detail_empty(screen, &papermono_icon_library_focal, "没有可显示的书籍",
			papermono_content_store_state_text(papermono_content_store_media_state()));
		(void)make_ink_shape(screen, 22, 532, 436, 1, ink, 0);
		detail_section(screen, 552, "支持格式");
		(void)detail_text(screen, "UTF-8 / GBK TXT\n无 DRM 的文字型 EPUB\n单本上限 64 MiB", 22, 596, 436, 90, false);
		make_footer(screen, "长按重新扫描");
		return 0;
	}
	(void)snprintf(text, sizeof(text), state->last_error < 0 ?
		"打开或扫描失败 · 仍显示 %u 本书" : "%u 本书%s",
		(unsigned)(state->last_error < 0 ? state->book_count : state->total_books),
		state->total_books > state->book_count ? " · 仅显示前 32 本" : "");
	(void)detail_text(screen, text, 22, 124, 436, 24, false);
	const size_t first = (state->selected / LIBRARY_VISIBLE_BOOKS) * LIBRARY_VISIBLE_BOOKS;
	for (size_t row = 0; row < LIBRARY_VISIBLE_BOOKS && first + row < state->book_count; ++row) {
		struct papermono_rect rect;
		const size_t index = first + row;
		(void)papermono_interaction_grid_cell_rect(&detail_list_grid, row, &rect);
		(void)snprintf(text, sizeof(text), "%s · %llu KiB",
			state->books[index].kind == PAPERMONO_CONTENT_TXT ? "TXT" : "EPUB",
			(unsigned long long)((state->books[index].size + 1023U) / 1024U));
		lv_obj_t *item = detail_row(screen, &rect, "", NULL, index == state->selected, true);
		(void)make_outline_shape(item, 18, 18, 30, 62, 1, 0);
		(void)make_ink_shape(item, 24, 18, 1, 62, ink, 0);
		(void)detail_text(item, state->books[index].title, 68, 10, 326, 34, true);
		(void)detail_text(item, text, 68, 54, 340, 24, false);
	}
	(void)snprintf(text, sizeof(text), "%02u / %02u · 轻触打开\n%s",
		(unsigned)(first / 5 + 1), (unsigned)((state->book_count + 4) / 5),
		state->last_error < 0 ? "文件可能损坏 · 长按重新扫描" : "长按重新扫描");
	make_footer(screen, text);
	return 0;
}

static int render_library_reader(struct library_ui_state *state,
	lv_obj_t *screen)
{
	const struct papermono_reader_layout layout = papermono_reader_layout_for(
		papermono_settings_get()->reader_density);
	const bool epub = state->reader.book.kind == PAPERMONO_CONTENT_EPUB;
	const struct papermono_rect *menu = &reader_regions[0].rect;
	lv_obj_t *label = make_label(screen, state->reader.book.title, &papermono_reading_24, ink);

	lv_obj_set_pos(label, 22, 22);
	lv_obj_set_size(label, 336, 34);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_t *button = make_outline_shape(screen, menu->x, menu->y,
		menu->width, menu->height, 1, 0);
	label = make_chinese_label(button, epub ? "目录" : "菜单");
	lv_obj_center(label);
	(void)make_ink_shape(screen, 22, 66, 436, 1, ink, 0);

	state->last_error = papermono_reader_read_page(&state->reader,
		state->page_text, sizeof(state->page_text), &state->page);
	if (state->last_error < 0) {
		lv_obj_t *card = make_outline_shape(screen, 22, 132, 436, 250, 0, 0);
		char error[48];
		(void)snprintf(error, sizeof(error), "读取失败（%d）", state->last_error);
		(void)detail_text(card, "无法读取书籍", 0, 0, 436, 38, true);
		(void)detail_text(card, error, 0, 54, 436, 64, false);
		(void)detail_text(card, papermono_content_store_state_text(papermono_content_store_media_state()), 0, 132, 436, 70, false);
	} else {
		label = make_label(screen, state->page_text, &papermono_reading_24, ink);
		lv_obj_set_pos(label, layout.margin, PAPERMONO_READER_BODY_TOP);
		lv_obj_set_size(label, UI_WIDTH - layout.margin * 2, PAPERMONO_READER_BODY_HEIGHT);
		lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_line_space(label, layout.line_space, LV_PART_MAIN);
	}
	(void)make_ink_shape(screen, 22, UI_FOOTER_TOP, 436, 1, ink, 0);
	footer_label = make_chinese_label(screen, "");
	if (state->last_error < 0) {
		lv_label_set_text(footer_label, "读取失败 · 轻触菜单");
	} else {
		/* No invented total: EPUB/TXT are indexed incrementally. */
		lv_label_set_text_fmt(footer_label, "第 %u 页 · %u%%",
			(unsigned int)(state->reader.page_number + 1), state->page.approximate_percent);
	}
	lv_obj_set_pos(footer_label, 22, 758);
	lv_obj_set_size(footer_label, 324, 24);
	label = make_chinese_label(screen, papermono_reader_is_bookmarked(&state->reader) ?
		"已加书签" : "书签");
	lv_obj_set_pos(label, 360, 758);
	lv_obj_set_size(label, 98, 24);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	(void)make_ink_shape(screen, 22, 790, 436, 1, ink, 0);
	if (state->last_error == 0 && state->page.approximate_percent > 0) {
		(void)make_ink_shape(screen, 22, 788,
			MAX(1, 436 * MIN(state->page.approximate_percent, 100) / 100), 4, ink, 0);
	}
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

static int render_library_menu(struct library_ui_state *state, lv_obj_t *screen)
{
	const struct papermono_preferences *prefs = papermono_settings_get();
	const char *titles[] = {
		papermono_reader_is_bookmarked(&state->reader) ? "移除当前页书签" : "添加当前页书签",
		"目录", "排版与编码", "前光亮度", "书签列表", "返回阅读", "返回书库",
	};
	char density[96], light[64], marks[48];
	(void)snprintf(density, sizeof(density), "%s · %s · %s",
		reader_density_name(prefs->reader_density), papermono_text_encoding_name(state->reader.encoding),
		state->reader.book.kind == PAPERMONO_CONTENT_TXT ? "长按切换编码" : "轻触切换密度");
	(void)snprintf(light, sizeof(light), "亮度 %u%%%s", prefs->frontlight_percent,
		frontlight_last_error < 0 ? " · 调整失败" : " · 长按关闭");
	(void)snprintf(marks, sizeof(marks), "%u 个位置", state->reader.bookmark_count);
	const char *meta[] = {NULL, state->reader.book.kind == PAPERMONO_CONTENT_EPUB ?
		"浏览并跳转章节" : "TXT 没有内置目录", density, light, marks, NULL, NULL};
	const lv_image_dsc_t *icons[] = {&papermono_icon_bookmark_small, &papermono_icon_toc_small,
		&papermono_icon_settings_small, &papermono_icon_sun_small, &papermono_icon_bookmark_small,
		&papermono_icon_book_small, &papermono_icon_library_small};
	make_header(screen, "阅读菜单");
	(void)detail_text(screen, state->reader.book.title, 22, 124, 436, 24, false);
	for (size_t row = 0; row < 7; ++row) {
		struct papermono_rect rect;
		(void)papermono_interaction_grid_cell_rect(&detail_menu_grid, row, &rect);
		(void)detail_icon_row(screen, &rect, icons[row], titles[row], meta[row], false,
			row != 1 || state->reader.book.kind == PAPERMONO_CONTENT_EPUB);
	}
	make_footer(screen, "进度与书签自动保存");
	return 0;
}

static int render_library_toc(struct library_ui_state *state, lv_obj_t *screen)
{
	make_header(screen, "目录");
	(void)detail_text(screen, state->reader.book.title, 22, 124, 436, 24, false);
	const uint16_t first = (state->auxiliary_selected / 5U) * 5U;
	const uint16_t count = state->reader.epub.toc_count > 0U ? state->reader.epub.toc_count : state->reader.epub.chapter_count;
	for (uint16_t row = 0; row < 5 && first + row < count; ++row) {
		const uint16_t index = first + row;
		const struct papermono_epub_toc_entry *toc = state->reader.epub.toc_count ? &state->reader.epub.toc[index] : NULL;
		const uint16_t chapter = toc ? toc->chapter : index;
		if (chapter >= state->reader.epub.chapter_count) break;
		struct papermono_rect rect;
		char meta[80];
		(void)papermono_interaction_grid_cell_rect(&detail_list_grid, row, &rect);
		const int32_t indent = toc ? MIN(toc->level, 4U) * 16 : 0;
		rect.x += indent; rect.width -= indent;
		(void)snprintf(meta, sizeof(meta), "%s · 章节 %u / %u",
			chapter == state->reader.chapter ? "当前章节" : "轻触跳转", chapter + 1U, state->reader.epub.chapter_count);
		(void)detail_row(screen, &rect, toc ? toc->title : state->reader.epub.chapters[chapter].title,
			meta, index == state->auxiliary_selected, true);
	}
	char footer[64];
	(void)snprintf(footer, sizeof(footer), "%02u / %02u · 轻触跳转", first / 5U + 1U, (count + 4U) / 5U);
	make_footer(screen, footer);
	return 0;
}

static int render_library_bookmarks(struct library_ui_state *state, lv_obj_t *screen)
{
	make_header(screen, "书签");
	(void)detail_text(screen, state->reader.book.title, 22, 124, 436, 24, false);
	if (state->reader.bookmark_count == 0) {
		detail_empty(screen, &papermono_icon_bookmark_focal, "尚无书签", "阅读时长按页面，\n或在阅读菜单中添加当前页书签。");
		make_footer(screen, "返回阅读菜单");
		return 0;
	}
	const uint16_t first = state->auxiliary_selected / 5U * 5U;
	for (uint16_t row = 0; row < 5 && first + row < state->reader.bookmark_count; ++row) {
		const uint16_t index = first + row;
		const struct papermono_reader_bookmark *mark = &state->reader.bookmarks[index];
		struct papermono_rect rect;
		char title[96], meta[96];
		(void)papermono_interaction_grid_cell_rect(&detail_list_grid, row, &rect);
		if (mark->preview[0]) (void)snprintf(title, sizeof(title), "%.*s",
			(int)sizeof(mark->preview), mark->preview);
		else (void)snprintf(title, sizeof(title), "书签 %u", index + 1U);
		if (state->reader.book.kind == PAPERMONO_CONTENT_EPUB && mark->chapter < state->reader.epub.chapter_count)
			(void)snprintf(meta, sizeof(meta), "%s · 位置 %u", state->reader.epub.chapters[mark->chapter].title, mark->offset);
		else (void)snprintf(meta, sizeof(meta), "文件位置 %u", mark->offset);
		(void)detail_icon_row(screen, &rect, &papermono_icon_bookmark_small, title, meta,
			index == state->auxiliary_selected, true);
	}
	make_footer(screen, "轻触跳转 · 左右键选择");
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

#if defined(CONFIG_PAPERMONO_BLE_MIC)
#define REMOTE_TAB_COUNT 4
#else
#define REMOTE_TAB_COUNT 3
#endif

static const struct papermono_interaction_grid_layout remote_tab_grid = {
	.x = 22,
	.y = 212,
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	/* Four 103 px cells and three 8 px gaps still span the 436 px content
	 * column, so the tab row keeps its y, height and right margin.
	 */
	.cell_width = 103,
#else
	.cell_width = 140,
#endif
	.cell_height = 64,
	.column_gap = 8,
	.row_gap = 0,
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	.columns = 4,
#else
	.columns = 3,
#endif
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
	22, 296, 436, 250,
};

#if defined(CONFIG_PAPERMONO_BLE_MIC)
/* The hold target is drawn and hit tested from this one rectangle. */
static const struct papermono_rect remote_voice_pad = {
	22, 296, 436, 250,
};
#endif

static void remote_action_card(lv_obj_t *screen, int column, int row,
	const char *title, const char *meta)
{
	struct papermono_rect rect;
	if (papermono_interaction_grid_cell_rect(&remote_action_grid,
		(size_t)row * remote_action_grid.columns + (size_t)column, &rect) < 0) return;
	lv_obj_t *button = make_outline_shape(screen, rect.x, rect.y, rect.width, rect.height, 1, 0);
	const int32_t x = rect.width / 2 - 20;
	/* Authored monochrome controls share the launcher's geometric vocabulary. */
	if (strcmp(meta, "PLAY") == 0 || strcmp(meta, "F5") == 0) {
		(void)make_fixed_line(button, x+8, 14, x+8, 48, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+8, 14, x+34, 31, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+34, 31, x+8, 48, 2, 0, 0, ink);
	} else if (strcmp(meta, "PREVIOUS") == 0 || strcmp(meta, "PAGE UP") == 0 || strcmp(meta, "NEXT") == 0 || strcmp(meta, "PAGE DOWN") == 0) {
		const bool previous = strcmp(meta, "PREVIOUS") == 0 || strcmp(meta, "PAGE UP") == 0;
		int32_t a = previous ? x+30 : x+10, b = previous ? x+10 : x+30;
		(void)make_fixed_line(button, a, 14, b, 31, 2, 0, 0, ink);
		(void)make_fixed_line(button, b, 31, a, 48, 2, 0, 0, ink);
		(void)make_fixed_line(button, a, 48, a, 14, 2, 0, 0, ink);
	} else if (strcmp(meta, "MUTE") == 0) {
		(void)make_outline_shape(button, x, 24, 12, 16, 2, 0);
		(void)make_fixed_line(button, x+12, 24, x+24, 14, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+24, 14, x+24, 48, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+24, 48, x+12, 40, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+32, 24, x+44, 40, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+44, 24, x+32, 40, 2, 0, 0, ink);
	} else if (strcmp(meta, "VOLUME -") == 0 || strcmp(meta, "VOLUME +") == 0) {
		(void)make_fixed_line(button, x, 31, x+40, 31, 3, 0, 0, ink);
		if (strcmp(meta, "VOLUME +") == 0) (void)make_fixed_line(button, x+20, 11, x+20, 51, 3, 0, 0, ink);
	} else if (strcmp(meta, "B") == 0) {
		(void)make_outline_shape(button, x, 14, 40, 28, 2, 0);
		(void)make_fixed_line(button, x+20, 42, x+20, 51, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+8, 51, x+32, 51, 2, 0, 0, ink);
	} else {
		(void)make_fixed_line(button, x+6, 16, x+34, 46, 2, 0, 0, ink);
		(void)make_fixed_line(button, x+34, 16, x+6, 46, 2, 0, 0, ink);
	}
	lv_obj_t *label = detail_text(button, title, 8, 66, rect.width-16, 34, true);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void remote_status_card(lv_obj_t *screen, int column, int row,
	const char *title, const char *body, const char *meta)
{
	ARG_UNUSED(meta);
	struct papermono_rect rect;
	if (papermono_interaction_grid_cell_rect(&remote_action_grid,
		(size_t)row * remote_action_grid.columns + (size_t)column, &rect) < 0) return;
	lv_obj_t *label = detail_text(screen, title, rect.x+8, rect.y+20, rect.width-16, 34, true);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	label = detail_text(screen, body, rect.x+8, rect.y+60, rect.width-16, 44, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static bool remote_ready(const struct papermono_hid_state *state)
{
	return state->initialized && state->connected && state->secured &&
		!state->suspended && !state->passkey_visible;
}

static void remote_touchpad_enable(bool enabled)
{
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI) && defined(CONFIG_PAPERMONO_BLE_HID)
	const atomic_val_t epoch = atomic_get(&remote_touchpad_epoch);

	if ((epoch & 1) != (int)enabled) {
		papermono_hid_mouse_enable(enabled);
		atomic_set(&remote_touchpad_epoch, epoch + 1);
	}
#else
	ARG_UNUSED(enabled);
#endif
}

static void __maybe_unused update_touchpad_activity(int64_t now)
{
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI) && defined(CONFIG_PAPERMONO_BLE_HID)
	const uint32_t last = (uint32_t)atomic_get(&remote_touchpad_activity);
	if (last != 0U && (atomic_get(&remote_touchpad_epoch) & 1) != 0) {
		last_activity_ms = MAX(last_activity_ms,
			now - (uint32_t)((uint32_t)now - last));
	}
#else
	ARG_UNUSED(now);
#endif
}

/* Arm or disarm hold-to-talk and stop whatever session the voice page owned.
 * Disarming is what stops the stream when the user leaves the page, switches
 * mode, loses the subscription or sleeps: the release event that would normally
 * stop it may belong to another page by then. Only the UI thread calls this,
 * so the epoch write needs no CAS.
 */
static void remote_voice_set_armed(bool armed)
{
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	const atomic_val_t epoch = atomic_get(&remote_voice_epoch);

	if ((epoch & 1) == (int)armed) {
		return;
	}
	atomic_set(&remote_voice_epoch, epoch + 1);
	if (!armed) {
		/* The page that could hold a session is gone, so retire the contact
		 * flag here instead of waiting for a release that may never arrive.
		 * request_stop() is idempotent and does nothing when idle.
		 */
		(void)atomic_cas(&remote_voice_contact, 1, 0);
		(void)papermono_ble_mic_request_stop();
	}
#else
	ARG_UNUSED(armed);
#endif
}

#if defined(CONFIG_PAPERMONO_BLE_MIC)
/* Report only what the microphone worker publishes. There is deliberately no
 * timer and no level meter: the page changes on start, on stop and on the
 * background refresh the worker requests when it stops a session itself.
 */
static void remote_voice_status(const struct papermono_ble_mic_state *mic,
	char *text, size_t size)
{
	const int request_error = (int)atomic_get(&remote_voice_start_error);
	const int error = mic->last_error < 0 ? mic->last_error : request_error;

	if (error < 0 || !mic->initialized) {
		(void)snprintf(text, size,
			"语音传输失败（%d）\n请确认电脑已连接并订阅",
			error < 0 ? error : -ENODEV);
		return;
	}
	if (!mic->subscribed) {
		(void)snprintf(text, size,
			"等待电脑订阅语音服务\n请在电脑端启动接收器");
		return;
	}
	switch (mic->phase) {
	case PAPERMONO_BLE_MIC_STARTING:
		(void)snprintf(text, size, "正在启动麦克风…");
		break;
	case PAPERMONO_BLE_MIC_STREAMING:
		(void)snprintf(text, size, "正在传音\n松开结束");
		break;
	case PAPERMONO_BLE_MIC_STOPPING:
		(void)snprintf(text, size, "正在停止麦克风…");
		break;
	case PAPERMONO_BLE_MIC_IDLE:
	default:
		if (mic->sessions > 0U) {
			(void)snprintf(text, size,
				"按住此区域说话\n上次传输 %u.%u 秒 · %u KB",
				mic->session_ms / 1000U,
				(mic->session_ms % 1000U) / 100U,
				mic->session_bytes / 1024U);
		} else {
			(void)snprintf(text, size, "按住此区域说话\n松开结束");
		}
		break;
	}
}

static void render_remote_voice(lv_obj_t *screen,
	const struct papermono_hid_state *state)
{
	struct papermono_ble_mic_state mic;
	char status[128];
	const int32_t center = UI_WIDTH / 2;
	lv_obj_t *label;

	papermono_ble_mic_get_state(&mic);
	if (mic.phase != PAPERMONO_BLE_MIC_IDLE) {
		/* An accepted request owns the page now; drop a stale local error. */
		atomic_set(&remote_voice_start_error, 0);
	}
	/* Publish availability only while this page is the one being drawn: the
	 * input callback captures the epoch at press time and cannot see a page
	 * change otherwise. This branch only runs after remote_ready() passed.
	 */
	remote_voice_set_armed(remote_ready(state) && mic.subscribed);

	(void)make_outline_shape(screen, remote_voice_pad.x, remote_voice_pad.y,
		remote_voice_pad.width, remote_voice_pad.height, 1, 0);
	label = detail_text(screen, "语音输入", remote_voice_pad.x + 16, 320,
		remote_voice_pad.width - 32, 34, true);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	/* Capsule, cradle, stem and base reuse the media cards' geometric
	 * vocabulary, so the glyph needs no bitmap asset or extra font.
	 */
	(void)make_outline_shape(screen, center - 14, 366, 28, 52, 2, 0);
	(void)make_fixed_line(screen, center - 28, 392, center - 28, 428, 2, 0, 0, ink);
	(void)make_fixed_line(screen, center - 28, 428, center + 28, 428, 2, 0, 0, ink);
	(void)make_fixed_line(screen, center + 28, 392, center + 28, 428, 2, 0, 0, ink);
	(void)make_fixed_line(screen, center, 428, center, 444, 2, 0, 0, ink);
	(void)make_fixed_line(screen, center - 18, 444, center + 18, 444, 2, 0, 0, ink);
	remote_voice_status(&mic, status, sizeof(status));
	label = detail_text(screen, status, remote_voice_pad.x + 16, 460,
		remote_voice_pad.width - 32, 70, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	/* The receiver and its virtual microphone run on the computer; the device
	 * cannot verify either, so the hint states a requirement, not a status.
	 */
	label = detail_text(screen, "电脑端需运行接收器，\n并选择虚拟麦克风作为输入。",
		remote_voice_pad.x, 570, remote_voice_pad.width, 60, false);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}
#endif

static int render_remote(void *context, lv_obj_t *screen)
{
	struct remote_ui_state *remote = context;
	struct papermono_hid_state state = {0};
	struct papermono_ui_snapshot snapshot;
	char body[192];
	make_header(screen, "遥控器");
#if defined(CONFIG_PAPERMONO_BLE_HID)
	papermono_hid_get_state(&state);
#endif
	if (state.passkey_visible) {
		(void)detail_text(screen, "在电脑上输入配对码", 22, 130, 436, 24, false);
		(void)snprintf(body, sizeof(body), "%06u", state.passkey);
		detail_number(screen, body, 22, 154, 436);
	} else {
		if (state.connected) (void)snprintf(body, sizeof(body), "已连接%s%s%s%s",
			state.secured ? " · 安全连接" : " · 正在完成配对", state.bonded ? " · 已保存主机" : "",
			state.suspended ? " · 主机已挂起" : "", remote->last_error < 0 ? " · 操作失败" : "");
		else if (state.advertising) (void)snprintf(body, sizeof(body), "等待电脑连接\n蓝牙名称：PaperMono C153");
		else (void)snprintf(body, sizeof(body), "%s", state.initialized ? "尚未连接电脑" : "蓝牙 HID 未启用");
		const int error = remote->last_error < 0 ? remote->last_error : state.last_error;
		if (error < 0) {
			const size_t used = strlen(body);
			(void)snprintf(body + used, sizeof(body) - used, "\n上次发送或连接失败（%d）", error);
		}
		(void)detail_text(screen, body, 22, 132, 436, 72, false);
	}
	read_snapshot(&snapshot);
#if defined(CONFIG_PAPERMONO_BLE_HID)
	/* Preserve the existing HID battery report; the UI shows no estimated %. */
	if (snapshot.battery_valid && state.initialized) {
		(void)papermono_hid_set_battery_level((uint8_t)CLAMP(((int)snapshot.battery_mv - 3300) * 100 / 900, 0, 100));
	}
#endif
	static const char *const tabs[REMOTE_TAB_COUNT] = {
		"媒体", "演示", "鼠标",
#if defined(CONFIG_PAPERMONO_BLE_MIC)
		"语音",
#endif
	};
	for (size_t i = 0; i < ARRAY_SIZE(tabs); ++i) {
		struct papermono_rect rect;
		(void)papermono_interaction_grid_cell_rect(&remote_tab_grid, i, &rect);
		lv_obj_t *label = detail_text(screen, tabs[i], rect.x, rect.y+14, rect.width, 34, true);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
		if (remote->mode == (enum remote_mode)i) (void)make_ink_shape(screen, rect.x+30, rect.y+rect.height-5, rect.width-60, 3, ink, 0);
	}
	(void)make_ink_shape(screen, 22, 280, 436, 1, ink, 0);
	if (!remote_ready(&state)) {
		/* The voice page is not visible, so hold-to-talk must not be armed. */
		remote_voice_set_armed(false);
		detail_icon(screen, &papermono_icon_remote, 340);
		lv_obj_t *label = detail_text(screen, state.suspended ? "等待电脑恢复" : "完成连接后可操作",
			22, 472, 436, 38, true);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
		label = detail_text(screen, state.passkey_visible ? "在电脑上输入上方配对码。\n完成安全配对后，遥控按钮才会启用。" :
			state.connected && !state.secured ? "正在完成安全配对。\n请在电脑蓝牙设置中确认。" :
			state.suspended ? "主机已挂起，当前无法发送指令。" :
			"在电脑的蓝牙设置中连接\nPaperMono C153。", 22, 534, 436, 70, false);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
		if (state.passkey_visible && remote->last_error < 0) {
			(void)snprintf(body, sizeof(body), "上次发送失败（%d）", remote->last_error);
			(void)detail_text(screen, body, 22, 626, 436, 24, false);
		}
		make_footer(screen, "顶部切换模式 · 连接后可使用");
		return 0;
	}
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	if (remote->mode == REMOTE_MODE_VOICE) {
		render_remote_voice(screen, &state);
		make_footer(screen, "按住说话 · 顶部切换模式");
		return 0;
	}
#endif
	if (remote->mode == REMOTE_MODE_MEDIA) {
		remote_action_card(screen,0,0,"播放 / 暂停","PLAY"); remote_action_card(screen,1,0,"静音","MUTE");
		remote_action_card(screen,0,1,"上一首","PREVIOUS"); remote_action_card(screen,1,1,"下一首","NEXT");
		remote_action_card(screen,0,2,"减小音量","VOLUME -"); remote_action_card(screen,1,2,"增大音量","VOLUME +");
	} else if (remote->mode == REMOTE_MODE_PRESENTATION) {
		remote_action_card(screen,0,0,"上一页","PAGE UP"); remote_action_card(screen,1,0,"下一页","PAGE DOWN");
		remote_action_card(screen,0,1,"开始放映","F5"); remote_action_card(screen,1,1,"退出放映","ESC");
		remote_action_card(screen,0,2,"黑屏切换","B");
		remote_status_card(screen,1,2,"连接状态", state.connected ? "已连接，可以发送指令" : "请先完成电脑配对", NULL);
	} else {
		(void)make_outline_shape(screen, remote_mouse_pad.x, remote_mouse_pad.y, remote_mouse_pad.width, remote_mouse_pad.height, 1, 0);
		(void)make_fixed_line(screen, 360, remote_mouse_pad.y, 360, remote_mouse_pad.y+remote_mouse_pad.height, 1, 0, 0, ink);
		(void)make_fixed_line(screen, 206, 392, 246, 392, 1, 0, 0, ink);
		(void)make_fixed_line(screen, 226, 372, 226, 412, 1, 0, 0, ink);
		lv_obj_t *label = detail_text(screen, "触摸板", 22, 450, 338, 34, true);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
		(void)detail_text(screen, "轻触：左键 · 长按：右键\n中部滑动：移动指针\n右侧上下滑动：滚轮\n实体键：左键 / 右键", 22, 570, 436, 110, false);
	}
	make_footer(screen, "顶部切换模式 · 轻触发送");
	return 0;
}

static int remote_send(struct remote_ui_state *remote,
	enum papermono_hid_action_type type, int8_t x, int8_t y, int8_t wheel,
	uint8_t buttons)
{
#if defined(CONFIG_PAPERMONO_BLE_HID)
	struct papermono_hid_state state;
	papermono_hid_get_state(&state);
	if (!remote_ready(&state)) {
		remote->last_error = state.suspended ? -EHOSTDOWN : state.connected ? -EACCES : -ENOTCONN;
		return remote->last_error;
	}
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
	     event->y >= UI_DETAIL_FOOTER_TOP)) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
		return 0;
	}
	if (papermono_interaction_spaced_grid_index(event,
		PAPERMONO_INTERACTION_TAP, &remote_tab_grid, &grid_index) == 0) {
		remote_touchpad_enable(false);
		/* Switching modes abandons the visible page, so a hold that started
		 * there must stop instead of waiting for a release that now belongs
		 * to the next mode.
		 */
		remote_voice_set_armed(false);
		remote->mode = (enum remote_mode)grid_index;
		request_redraw(command);
		return 0;
	}

	struct papermono_hid_state state = {0};
#if defined(CONFIG_PAPERMONO_BLE_HID)
	papermono_hid_get_state(&state);
#endif
	if (!remote_ready(&state)) return 0;

#if defined(CONFIG_PAPERMONO_BLE_MIC)
	if (remote->mode == REMOTE_MODE_VOICE) {
		/* Hold-to-talk is owned by the input callback so the microphone is
		 * started and stopped without waiting for gesture classification.
		 * Taps on the page therefore send nothing.
		 */
		return 0;
	}
#endif

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
		}
	}
	if (ret < 0) {
		LOG_WRN("HID action failed (%d)", ret);
		request_redraw(command);
	}
	return 0;
}

/* Leaving the remote page (back, home, footer, app switch) reaches this hook,
 * so a hold in progress stops here even though its release will be delivered
 * to whatever page is showing by then.
 */
static void remote_close(void *context)
{
	ARG_UNUSED(context);
	remote_touchpad_enable(false);
	remote_voice_set_armed(false);
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
	struct papermono_ui_snapshot snapshot;
	make_header(screen, "日历");
	if (!count) {
		detail_empty(screen, &papermono_icon_calendar_focal, "暂无日程", "通过内容中心上传 .ics 文件。\n支持全天日程与基础重复规则。");
		make_footer(screen, "右滑返回");
		return 0;
	}
	read_snapshot(&snapshot);
	char date[64];
	(void)snprintf(date, sizeof(date), snapshot.rtc_valid ? "%02d" : "--", snapshot.time.tm_mday);
	lv_obj_t *label = make_label(screen, date, &papermono_clock_148, ink);
	lv_obj_set_pos(label, 22, 132); lv_obj_set_size(label, 200, 122);
	if (snapshot.rtc_valid) {
		(void)snprintf(date, sizeof(date), "%d 年 %02d 月", snapshot.time.tm_year + 1900, snapshot.time.tm_mon + 1);
	} else (void)snprintf(date, sizeof(date), "时钟尚未校准");
	(void)detail_text(screen, date, 234, 190, 224, 34, true);
	if (ui->calendar_first >= count) ui->calendar_first = (count - 1U) / 5U * 5U;
	for (size_t row = 0; row < 5 && ui->calendar_first + row < count; ++row) {
		struct papermono_calendar_event event;
		char meta[96];
		if (papermono_organizer_get_event(ui->calendar_first + row, &event) < 0) break;
		const struct papermono_rect rect = {22, 272 + (int32_t)row * 86, 436, 84};
		if (event.start.all_day) (void)snprintf(meta, sizeof(meta), "%04d-%02u-%02u · 全天", event.start.year, event.start.month, event.start.day);
		else (void)snprintf(meta, sizeof(meta), "%04d-%02u-%02u · %02u:%02u", event.start.year, event.start.month, event.start.day, event.start.hour, event.start.minute);
		const char *repeat = recurrence_name(event.recurrence);
		if (repeat) {
			const size_t used = strlen(meta);
			(void)snprintf(meta + used, sizeof(meta) - used, " · %s", repeat);
		}
		(void)detail_row(screen, &rect, event.title, meta, false, false);
	}
	(void)snprintf(date, sizeof(date), "%02u / %02u · 实体键翻页", (unsigned)(ui->calendar_first / 5 + 1), (unsigned)((count + 4) / 5));
	make_footer(screen, date);
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
		    event->y >= UI_DETAIL_FOOTER_TOP)) {
		/* This list is the app root, so back leaves the app. Handling
		 * EVENT_BACK explicitly matters because the edge-swipe gesture
		 * is otherwise swallowed and leaves the user stranded.
		 */
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

#define organizer_list_grid detail_list_grid

static int render_tasks(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_organizer_task_count();
	char text[96];
	make_header(screen, "待办");
	if (!count) {
		detail_empty(screen, &papermono_icon_tasks_focal, "暂无待办", "通过内容中心上传待办资料。\ntasks-v1.json");
		make_footer(screen, "右滑返回");
		return 0;
	}
	if (ui->task_first >= count) ui->task_first = (count - 1U) / 5U * 5U;
	(void)snprintf(text, sizeof(text), "%u 项待办", (unsigned)count);
	(void)detail_text(screen, text, 22, 124, 436, 24, false);
	for (size_t row = 0; row < 5 && ui->task_first + row < count; ++row) {
		struct papermono_task task;
		struct papermono_rect rect;
		if (papermono_organizer_get_task(ui->task_first + row, &task) < 0) break;
		(void)papermono_interaction_grid_cell_rect(&detail_list_grid, row, &rect);
		lv_obj_t *item = detail_row(screen, &rect, "", NULL, false, false);
		make_checkbox(item, 12, 15, task.done);
		(void)detail_text(item, task.title, 50, 10, 372, 34, true);
		(void)snprintf(text, sizeof(text), task.done ? "已完成" : "%s · 优先级 %d", task.due[0] ? task.due : "无截止日期", task.priority);
		(void)detail_text(item, text, 50, 54, 372, 24, false);
	}
	(void)snprintf(text, sizeof(text), "%02u / %02u · 轻触勾选", (unsigned)(ui->task_first / 5 + 1), (unsigned)((count + 4) / 5));
	make_footer(screen, text);
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
		    event->y >= UI_DETAIL_FOOTER_TOP)) {
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
	bool favorite = false;
	make_header(screen, ui->note_open ? "便签内容" : "便签");
	if (ui->note_open && papermono_organizer_get_note(ui->note_selected, &note_view, &favorite, NULL) == 0) {
		(void)detail_text(screen, note_view.title, 22, 140, 436, 38, true);
		if (favorite) (void)make_ink_shape(screen, 442, 130, 12, 18, ink, 0);
		int32_t y = 200;
		for (size_t index = 0; index < note_view.block_count && y <= 670; ++index) {
			const struct papermono_note_block *block = &note_view.blocks[index];
			if (block->type == PAPERMONO_NOTE_SEPARATOR) {
				(void)make_ink_shape(screen, 22, y + 8, 436, 1, ink, 0);
				y += 30; continue;
			}
			char text[180];
			(void)snprintf(text, sizeof(text), "%s%s", note_block_prefix(block->type), block->text);
			const bool quote = block->type == PAPERMONO_NOTE_QUOTE;
			lv_obj_t *label = make_label(screen, text, &papermono_reading_24, ink);
			lv_obj_set_pos(label, quote ? 40 : 22, y);
			lv_obj_set_width(label, quote ? 418 : 436);
			lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
			lv_obj_set_style_text_line_space(label, 6, LV_PART_MAIN);
			lv_obj_update_layout(label);
			const int32_t height = MAX(lv_obj_get_height(label), 30);
			const int32_t available = (700 - y + 6) / 36 * 36 - 6;
			if (quote) (void)make_ink_shape(screen, 22, y, 3, MIN(height, available), ink, 0);
			if (height > available) { lv_obj_set_height(label, available); y = 700; }
			else y += height + (block->type == PAPERMONO_NOTE_HEADING ? 24 : 18);
		}
		make_footer(screen, favorite ? "已收藏 · 长按切换收藏" : "长按切换收藏");
		return 0;
	}
	ui->note_open = false;
	if (!count) {
		detail_empty(screen, &papermono_icon_document_focal, "暂无便签", "通过内容中心上传 Markdown 或 TXT。\n支持标题、段落、列表与引用。");
	} else {
		if (ui->note_first >= count) ui->note_first = (count - 1U) / 5U * 5U;
		char text[96];
		(void)snprintf(text, sizeof(text), "%u 篇便签", (unsigned)count);
		(void)detail_text(screen, text, 22, 124, 436, 24, false);
		for (size_t row = 0; row < 5 && ui->note_first + row < count; ++row) {
			struct papermono_rect rect;
			bool read;
			if (papermono_organizer_get_note(ui->note_first + row, &note_view, &favorite, &read) < 0) break;
			(void)papermono_interaction_grid_cell_rect(&detail_list_grid, row, &rect);
			(void)snprintf(text, sizeof(text), "%s%s", favorite ? "已收藏 · " : "", read ? "已读" : "未读");
			lv_obj_t *item = detail_row(screen, &rect, "", NULL, false, true);
			detail_image_at(item, &papermono_icon_document_small, 12, 12);
			(void)detail_text(item, note_view.title, 64, 10, 330, 34, true);
			(void)detail_text(item, text, 64, 54, 348, 24, false);
		}
	}
	make_footer(screen, "轻触打开 · 长按收藏");
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
			    event->y >= UI_DETAIL_FOOTER_TOP)) {
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
		    event->y >= UI_DETAIL_FOOTER_TOP)) {
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

/* Show the provider's Unix timestamp in explicit UTC without depending on
 * the RTC being set, or assigning a timezone that the source never supplied.
 */
static void format_info_time(char *output, size_t size, int64_t timestamp)
{
	if (timestamp <= 0 || timestamp > 253402300799LL) {
		(void)snprintf(output, size, "--");
		return;
	}
	const int64_t days = timestamp / 86400 + 719468;
	const int64_t era = days / 146097;
	const unsigned doe = (unsigned)(days - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned day = doy - (153 * mp + 2) / 5 + 1;
	const unsigned month = mp < 10 ? mp + 3 : mp - 9;
	const int year = (int)(yoe + era * 400) + (month <= 2);
	(void)snprintf(output, size, "%04d-%02u-%02u %02u:%02u UTC", year, month,
		day, (unsigned)(timestamp % 86400 / 3600), (unsigned)(timestamp % 3600 / 60));
}

static int render_info(void *context, lv_obj_t *screen)
{
	struct organizer_ui_state *ui = context;
	const size_t count = papermono_info_count();
	make_header(screen, "信息卡片");
	if (!count) {
		detail_empty(screen, &papermono_icon_document_focal, "尚未配置数据源", "添加数据源后，将显示标题、摘要和更新时间。");
		(void)make_ink_shape(screen, 22, 552, 436, 1, ink, 0);
		detail_section(screen, 576, "离线时");
		(void)detail_text(screen, "保留最近缓存，并显示更新时间。", 22, 624, 436, 52, false);
		make_footer(screen, "等待内容更新");
		return 0;
	}
	if (ui->info_first >= count) ui->info_first = (count - 1U) / 4U * 4U;
	for (size_t row = 0; row < 4 && ui->info_first + row < count; ++row) {
		struct papermono_info_card info;
		char title[160], meta[128], updated[32];
		if (papermono_info_get(ui->info_first + row, &info) < 0) break;
		const int32_t y = 148 + (int32_t)row * 138;
		(void)snprintf(title, sizeof(title), info.has_value ? "%s · %s" : "%s", info.title, info.value);
		detail_image_at(screen, &papermono_icon_info_small, 22, y);
		(void)detail_text(screen, title, 86, y, 372, 34, true);
		(void)detail_text(screen, info.summary, 86, y + 42, 372, 48, false);
		format_info_time(updated, sizeof(updated), info.updated_at);
		(void)snprintf(meta, sizeof(meta), "%s · %s · %s", info.source,
			info_status_name(info.status), updated);
		(void)detail_text(screen, meta, 22, y + 96, 436, 24, false);
		(void)make_ink_shape(screen, 22, y + 130, 436, 1, ink, 0);
	}
	char footer[64];
	(void)snprintf(footer, sizeof(footer), "%02u / %02u · 实体键翻页", (unsigned)(ui->info_first / 4 + 1), (unsigned)((count + 3) / 4));
	make_footer(screen, footer);
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
		    event->y >= UI_DETAIL_FOOTER_TOP)) {
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
	ARG_UNUSED(context);
	const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
	const struct device *const nfc = DEVICE_DT_GET(DT_NODELABEL(nfc));
	const struct device *const lora = DEVICE_DT_GET(DT_NODELABEL(lora));
	const struct device *const mic = DEVICE_DT_GET(DT_NODELABEL(microphone));
	struct papermono_ft6336_status touch_status = {0};
	struct papermono_sync_state sync = {0};
	enum papermono_wakeup_cause cause = PAPERMONO_WAKE_OTHER;
	char body[192];
	make_header(screen, "硬件工具");
	int touch_ret = papermono_ft6336_get_status(&touch_status);
	int wake_ret = papermono_power_get_wakeup_cause(&cause);
	(void)snprintf(body, sizeof(body), "%s · X %d Y %d · %s\n帧 %u · I2C %u · 异常 %u · 唤醒 %s",
		touch_ret == 0 && touch_status.online ? "在线" : "等待恢复",
		touch_status.x, touch_status.y, touch_status.pressed ? "按下" : "松开",
		touch_status.frames, touch_status.read_errors, touch_status.invalid_frames,
		wake_ret == 0 ? papermono_wakeup_cause_name(cause) : "--");
	(void)detail_row(screen, &detail_system_rect, "系统与触摸", body, false, true);
	const int sample = device_is_ready(imu) ? sensor_sample_fetch(imu) : -ENODEV;
	const char *titles[] = {"TF 存储", "IMU", "NFC", "LoRa", "音频与提示", "BLE 小数据同步"};
	for (size_t row = 0; row < 6; ++row) {
		const struct papermono_rect rect = {22, 260 + (int32_t)row * 74, 436, 72};
		switch (row) {
		case 0: (void)snprintf(body, sizeof(body), "%s", papermono_content_store_state_text(papermono_content_store_media_state())); break;
		case 1: (void)snprintf(body, sizeof(body), "%s · %s", ready_text(imu), sample == 0 ? "采样成功" : "尚无有效采样"); break;
		case 2: (void)snprintf(body, sizeof(body), "%s · 射频未验收", ready_text(nfc)); break;
		case 3: (void)snprintf(body, sizeof(body), "%s · 默认禁止发射", ready_text(lora)); break;
		case 4: (void)snprintf(body, sizeof(body), "麦克风 %s · %s", ready_text(mic), status_led_text(papermono_status_led_get_state())); break;
		default:
#if defined(CONFIG_PAPERMONO_BLE_SYNC)
			papermono_sync_get_state(&sync);
#endif
			if (sync.active) (void)snprintf(body, sizeof(body), "接收 %llu / %llu 字节", (unsigned long long)sync.received, (unsigned long long)sync.expected);
			else (void)snprintf(body, sizeof(body), "%s · 错误 %d", sync.initialized ? "认证服务已就绪" : "同步服务未启用", sync.last_error);
		}
		(void)detail_row(screen, &rect, titles[row], body, false, false);
	}
	make_footer(screen, "轻触首项查看系统状态");
	return 0;
}

static int tools_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	const struct papermono_rect system_rect = detail_system_rect;

	ARG_UNUSED(context);
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&system_rect)) {
		(void)papermono_content_store_poll();
		(void)papermono_organizer_reload();
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_SYSTEM;
	} else if (papermono_app_event_is_back_navigation(event) ||
		   (event->type == PAPERMONO_APP_EVENT_TAP &&
		    event->y >= UI_DETAIL_FOOTER_TOP)) {
		command->type = PAPERMONO_APP_COMMAND_BACK;
	}
	return 0;
}

static int render_power(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	char text[128];
	make_header(screen, confirmation == CONFIRM_RESET_WIFI || confirmation == CONFIRM_RESET_BLE ?
		"连接设置" : confirmation == CONFIRM_RESET_SETTINGS ? "设备设置" :
		confirmation == CONFIRM_FORMAT_TF ? "存储" : "电源");
	if (confirmation != CONFIRM_NONE) {
		const char *title = confirmation == CONFIRM_SLEEP ? "确认休眠？" :
			confirmation == CONFIRM_SHUTDOWN ? "确认关机？" :
			confirmation == CONFIRM_RESET_WIFI ? "确认重新配网？" :
			confirmation == CONFIRM_RESET_BLE ? "确认清除配对？" :
			confirmation == CONFIRM_FORMAT_TF ? "格式化 TF 卡？" :
			"恢复默认设置？";
		const char *body = confirmation == CONFIRM_SLEEP ? "屏幕将保留当前画面。\n触摸或电源键唤醒。" :
			confirmation == CONFIRM_SHUTDOWN ? "设备将完全关机。\n仅可通过电源键重新启动。" :
			confirmation == CONFIRM_RESET_WIFI ? "将清除已保存的网络配置。\n内容文件不会删除。" :
			confirmation == CONFIRM_RESET_BLE ? "将删除已保存的蓝牙主机。\n内容文件不会删除。" :
			confirmation == CONFIRM_FORMAT_TF ? "卡上全部分区和文件都会被删除。\n此操作不可撤销。" :
			"偏好设置将恢复为默认值。\n内容文件不会删除。";
		const lv_image_dsc_t *icon = confirmation == CONFIRM_RESET_SETTINGS ||
			confirmation == CONFIRM_FORMAT_TF ? &papermono_icon_restore :
			confirmation == CONFIRM_RESET_WIFI ? &papermono_icon_wifi :
			confirmation == CONFIRM_RESET_BLE ? &papermono_icon_bluetooth :
			confirmation == CONFIRM_SLEEP ? &papermono_icon_sleep : &papermono_icon_power;
		const char *action = confirmation == CONFIRM_SLEEP ? "确认休眠" :
			confirmation == CONFIRM_SHUTDOWN ? "确认关机" : confirmation == CONFIRM_RESET_WIFI ?
			"确认重新配网" : confirmation == CONFIRM_RESET_BLE ? "确认清除配对" :
			confirmation == CONFIRM_FORMAT_TF ? "确认格式化" : "确认恢复";
		detail_confirmation(screen, icon, title, body, action, "取消可返回上一页");
		return 0;
	}
	detail_power_option(screen, &detail_sleep_rect, &papermono_icon_sleep, "立即休眠", "保留当前墨水屏画面。\n触摸或电源键唤醒。");
	detail_power_option(screen, &detail_shutdown_rect, &papermono_icon_power, "完全关机", "关闭系统电源。\n使用电源键重新启动。");
	detail_section(screen, 580, power_last_error < 0 ? "电源操作已取消" : "自动休眠");
	if (power_last_error < 0) (void)snprintf(text, sizeof(text), "上次操作未执行（%d）。\n松开屏幕后重试。", power_last_error);
	else {
		const unsigned minutes = papermono_settings_get()->idle_sleep_minutes;
		if (minutes) (void)snprintf(text, sizeof(text), "%u 分钟 · 在设置中调整休眠时间。", minutes);
		else (void)snprintf(text, sizeof(text), "已关闭 · 在设置中调整休眠时间。");
	}
	(void)detail_text(screen, text, 22, 632, 436, 64, false);
	make_footer(screen, "操作需要二次确认");
	return 0;
}

static const char *sleep_summary_text(enum papermono_sleep_summary summary)
{
	switch (summary) {
	case PAPERMONO_SLEEP_SUMMARY_HOURLY_WEATHER: return "未来 8 小时";
	case PAPERMONO_SLEEP_SUMMARY_READING: return "阅读进度";
	case PAPERMONO_SLEEP_SUMMARY_TOMORROW: return "明日预告";
	case PAPERMONO_SLEEP_SUMMARY_BLANK: return "留白";
	case PAPERMONO_SLEEP_SUMMARY_QUOTE:
	default: return "摘抄";
	}
}

static int render_settings(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	const struct papermono_preferences *prefs = papermono_settings_get();
	char value[96];
	make_header(screen, "设置");
	detail_section(screen, 134, "日常偏好");
	detail_section(screen, 402, "连接");
	detail_section(screen, 554, "设备");
	const char *titles[] = {"自动休眠", "时间格式", "前光亮度", "休眠屏摘要",
		"Wi-Fi 网络", "清除蓝牙配对", "恢复默认设置", "电源选项"};
	/* The loop below walks detail_setting_rects and indexes titles with the
	 * same counter, so the two tables have to stay the same length.
	 */
	_Static_assert(sizeof(titles) / sizeof(titles[0]) ==
		sizeof(detail_setting_rects) / sizeof(detail_setting_rects[0]),
		"settings titles must match detail_setting_rects");
	for (size_t i = 0; i < ARRAY_SIZE(detail_setting_rects); ++i) {
		const struct papermono_rect *rect = &detail_setting_rects[i];
		(void)detail_text(screen, titles[i], rect->x, rect->y + 10, i <= 3 ? 254 : 398, 34, true);
		(void)make_ink_shape(screen, rect->x, rect->y + rect->height - 1, rect->width, 1, ink, 0);
		if (i != DETAIL_SETTING_FRONTLIGHT) make_chevron(screen, 443, rect->y + 20);
		value[0] = '\0';
		if (i == DETAIL_SETTING_SLEEP) {
			if (prefs->idle_sleep_minutes) (void)snprintf(value, sizeof(value), "%u 分钟", prefs->idle_sleep_minutes);
			else (void)snprintf(value, sizeof(value), "已关闭");
		} else if (i == DETAIL_SETTING_TIME) (void)snprintf(value, sizeof(value), "%s", prefs->use_24_hour ? "24 小时制" : "12 小时制");
		else if (i == DETAIL_SETTING_FRONTLIGHT) (void)snprintf(value, sizeof(value), "%u%%", prefs->frontlight_percent);
		else if (i == DETAIL_SETTING_SUMMARY) (void)snprintf(value, sizeof(value), "%s", sleep_summary_text(prefs->sleep_summary));
		if (value[0]) {
			lv_obj_t *label = detail_text(screen, value, 276, rect->y + 15, 156, 24, false);
			lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
		}
	}
	(void)make_ink_shape(screen, 40, 316, 384, 1, ink, 0);
	for (int i = 0; i < 5; ++i) {
		(void)make_outline_shape(screen, 34 + i * 96, 310, 12, 12, 1, 6);
		if ((int)prefs->frontlight_percent / 25 == i) (void)make_ink_shape(screen, 34 + i * 96, 310, 12, 12, ink, 6);
	}
	(void)detail_text(screen, "长按后确认", 306, 620, 130, 24, false);
	make_footer(screen, settings_last_error < 0 ? "恢复默认设置失败 · 未更改" :
		frontlight_last_error < 0 ? "前光调整失败 · 未保存" : "轻触修改 · 前光长按关闭");
	return 0;
}

static int render_system(void *context, lv_obj_t *screen)
{
	ARG_UNUSED(context);
	struct papermono_ui_snapshot snapshot;
	char value[96];
	read_snapshot(&snapshot);
	make_header(screen, "系统状态");
	(void)detail_text(screen, "PaperMono C153", 22, 132, 436, 24, false);
	detail_section(screen, 174, "固件");
	detail_value(screen, 220, "处理器", "ESP32-S3");
	detail_value(screen, 252, "系统", "Zephyr " KERNEL_VERSION_STRING);
	detail_value(screen, 284, "界面", "LVGL 9");
	detail_value(screen, 316, "屏幕", "SSD1677 · 480 × 800");
	(void)snprintf(value, sizeof(value), "%u 分钟", snapshot.uptime_minutes);
	detail_value(screen, 348, "运行时间", value);
	(void)make_ink_shape(screen, 22, 392, 436, 1, ink, 0);
	detail_section(screen, 412, "电源");
	format_voltage(value, sizeof(value), snapshot.battery_valid, snapshot.battery_mv); detail_value(screen, 458, "电池电压", value);
	format_voltage(value, sizeof(value), snapshot.input_valid, snapshot.input_mv); detail_value(screen, 490, "输入电压", value);
	format_voltage(value, sizeof(value), snapshot.output_valid, snapshot.output_mv); detail_value(screen, 522, "输出电压", value);
	detail_value(screen, 554, "电源来源", snapshot.power_source_valid ? "已读取" : "--");
	(void)make_ink_shape(screen, 22, 596, 436, 1, ink, 0);
	detail_section(screen, 614, "存储");
	const enum papermono_media_state media = papermono_content_store_media_state();
	(void)detail_text(screen,
		storage_last_error < 0 ? "格式化失败" :
		papermono_content_store_state_text(media), 22, 650, 436, 28, false);
	if (media == PAPERMONO_MEDIA_UNFORMATTED) {
		/* The only way to make an exFAT/GPT card usable without a PC. */
		detail_button(screen, &system_storage_rect, "格式化 TF 卡", true);
	} else {
		detail_value(screen, 688, "NVS", "192 KiB");
	}
	make_footer(screen, media == PAPERMONO_MEDIA_UNFORMATTED ?
		"轻触格式化 · 会删除卡内全部文件" : "右滑返回");
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

#define library_book_grid detail_list_grid

#define library_menu_grid detail_menu_grid

#define library_auxiliary_grid detail_list_grid

static int library_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct library_ui_state *state = context;
	if (state->mode != LIBRARY_MODE_READER &&
	    event->type == PAPERMONO_APP_EVENT_TAP &&
	    event->y >= UI_DETAIL_FOOTER_TOP) {
		if (state->mode == LIBRARY_MODE_LIST) {
			command->type = PAPERMONO_APP_COMMAND_BACK;
		} else {
			state->mode = state->mode == LIBRARY_MODE_MENU ?
				LIBRARY_MODE_READER : LIBRARY_MODE_MENU;
			request_redraw(command);
		}
		return 0;
	}

	if (event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED &&
	    state->mode == LIBRARY_MODE_LIST) {
		(void)library_open(state);
		request_redraw(command);
		return 0;
	}

	if (state->mode == LIBRARY_MODE_LIST) {
		const struct papermono_rect empty_rescan_rect = library_rescan_rect;
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
	} else if (event->type == PAPERMONO_APP_EVENT_LONG_PRESS ||
		   (touch_routed && touch_action == LIBRARY_TOUCH_BOOKMARK)) {
		(void)papermono_reader_toggle_bookmark(&state->reader,
			state->page_text);
		request_redraw(command);
	} else if (touch_routed && touch_action == LIBRARY_TOUCH_OPEN_TOC &&
		   state->reader.book.kind == PAPERMONO_CONTENT_EPUB) {
		state->auxiliary_selected = 0U;
		for (uint16_t toc = 0; toc < state->reader.epub.toc_count; ++toc) {
			if (state->reader.epub.toc[toc].chapter == state->reader.chapter) {
				state->auxiliary_selected = toc;
				break;
			}
		}
		state->mode = LIBRARY_MODE_TOC;
		request_redraw(command);
	} else if (touch_routed && (touch_action == LIBRARY_TOUCH_OPEN_MENU ||
				  touch_action == LIBRARY_TOUCH_OPEN_TOC)) {
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
	} else if (papermono_interaction_route(event, navigation_regions,
			ARRAY_SIZE(navigation_regions), &action, NULL) == 0) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = (enum papermono_app_id)action;
	} else if (papermono_interaction_route(event, home_regions,
			ARRAY_SIZE(home_regions), &action, NULL) == 0) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = (enum papermono_app_id)action;
		if (action == PAPERMONO_APP_LIBRARY) {
			struct papermono_content_entry recent;
			library_resume_requested = papermono_reader_recent(&recent) == 0;
		}
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
	uint16_t action;

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
	} else if (papermono_interaction_route(event, navigation_regions,
			ARRAY_SIZE(navigation_regions), &action, NULL) == 0) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = (enum papermono_app_id)action;
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
		0, UI_DETAIL_FOOTER_TOP, UI_WIDTH, UI_HEIGHT - UI_DETAIL_FOOTER_TOP,
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
		&weread_import_rect)) {
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
		0, UI_DETAIL_FOOTER_TOP, UI_WIDTH, UI_HEIGHT - UI_DETAIL_FOOTER_TOP,
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
	const struct papermono_rect confirm_rect = detail_confirm_rect;
	const struct papermono_rect cancel_rect = detail_cancel_rect;
	const struct papermono_rect sleep_rect = detail_sleep_rect;
	const struct papermono_rect shutdown_rect = detail_shutdown_rect;

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

				/* Record the reset failure on its own path. The
				 * frontlight is re-applied afterwards because the
				 * reset restores the default brightness, but its
				 * status says nothing about whether the reset
				 * itself worked.
				 */
				settings_last_error = reset_ret < 0 ? reset_ret : 0;
				if (reset_ret < 0) {
					LOG_ERR("Unable to restore default settings (%d)",
						reset_ret);
				}
				frontlight_last_error = apply_frontlight();
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
			} else if (confirmation == CONFIRM_FORMAT_TF) {
				const int format_ret =
					papermono_content_store_format();

				storage_last_error = format_ret;
				if (format_ret < 0) {
					LOG_ERR("Unable to format TF card (%d)",
						format_ret);
				} else {
					LOG_WRN("TF card formatted from the device UI");
					papermono_ui_request_content_refresh();
				}
				confirmation = CONFIRM_NONE;
				command->type = PAPERMONO_APP_COMMAND_BACK;
			} else {
				LOG_INF("Confirmed state cleanup request %d", confirmation);
				confirmation = CONFIRM_NONE;
				request_redraw(command);
			}
		} else if (papermono_app_event_is_back_navigation(event) ||
			   papermono_interaction_contains(event,
				   PAPERMONO_INTERACTION_TAP, &cancel_rect) ||
			   (event->type == PAPERMONO_APP_EVENT_TAP && event->y >= UI_DETAIL_FOOTER_TOP)) {
			confirmation = CONFIRM_NONE;
			if (confirmation_origin == PAPERMONO_APP_SETTINGS ||
			    confirmation_origin == PAPERMONO_APP_SYSTEM) command->type = PAPERMONO_APP_COMMAND_BACK;
			else request_redraw(command);
		}
		return 0;
	}
	if (papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&sleep_rect)) {
		confirmation_origin = PAPERMONO_APP_POWER;
		confirmation = CONFIRM_SLEEP;
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &shutdown_rect)) {
		confirmation_origin = PAPERMONO_APP_POWER;
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
	confirmation_origin = PAPERMONO_APP_POWER;
}

static int settings_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	const struct papermono_rect sleep_rect = detail_setting_rects[DETAIL_SETTING_SLEEP];
	const struct papermono_rect time_rect = detail_setting_rects[DETAIL_SETTING_TIME];
	const struct papermono_rect frontlight_rect = detail_setting_rects[DETAIL_SETTING_FRONTLIGHT];
	const struct papermono_rect wifi_rect = detail_setting_rects[DETAIL_SETTING_WIFI];
	const struct papermono_rect summary_rect = detail_setting_rects[DETAIL_SETTING_SUMMARY];
	const struct papermono_rect ble_rect = detail_setting_rects[DETAIL_SETTING_BLE];
	const struct papermono_rect reset_rect = detail_setting_rects[DETAIL_SETTING_RESET];
	const struct papermono_rect power_rect = detail_setting_rects[DETAIL_SETTING_POWER];
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
		PAPERMONO_INTERACTION_TAP, &summary_rect)) {
		const enum papermono_sleep_summary next =
			(prefs->sleep_summary + 1U) % PAPERMONO_SLEEP_SUMMARY_COUNT;

		(void)papermono_settings_set_sleep_summary(next);
		request_redraw(command);
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &wifi_rect)) {
		command->type = PAPERMONO_APP_COMMAND_OPEN;
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		command->target = PAPERMONO_APP_WIFI;
#else
		command->target = PAPERMONO_APP_CONTENT;
#endif
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_TAP, &ble_rect)) {
		confirmation_origin = PAPERMONO_APP_SETTINGS;
		confirmation = CONFIRM_RESET_BLE;
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
	} else if (papermono_interaction_contains(event,
		PAPERMONO_INTERACTION_LONG_PRESS, &reset_rect)) {
		confirmation_origin = PAPERMONO_APP_SETTINGS;
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

static int system_event(void *context,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	ARG_UNUSED(context);
	/* Offer formatting only when the card answered at the SD layer but carries
	 * no mountable filesystem; there is nothing to format when it is absent. */
	if (papermono_content_store_media_state() == PAPERMONO_MEDIA_UNFORMATTED &&
	    papermono_interaction_contains(event, PAPERMONO_INTERACTION_TAP,
		&system_storage_rect)) {
		confirmation_origin = PAPERMONO_APP_SYSTEM;
		confirmation = CONFIRM_FORMAT_TF;
		command->type = PAPERMONO_APP_COMMAND_OPEN;
		command->target = PAPERMONO_APP_POWER;
		return 0;
	}
	return common_event(context, event, command);
}

static const struct papermono_app_descriptor apps[] = {
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	{ PAPERMONO_APP_WIFI, "Wi-Fi", false, wifi_open, render_wifi, wifi_event, wifi_close, NULL },
#endif
	{ PAPERMONO_APP_HOME, "首页", true, NULL, render_home, home_event, NULL, NULL },
	{ PAPERMONO_APP_LAUNCHER, "全部应用", true, NULL, render_launcher, launcher_event, NULL, NULL },
	{ PAPERMONO_APP_LIBRARY, "书库", true, library_open, render_library, library_event, library_close, &library_state },
	{ PAPERMONO_APP_WEREAD, "微信读书", true, weread_open, render_weread, weread_event, NULL, &weread_ui },
	{ PAPERMONO_APP_CONTENT, "内容中心", true, NULL, render_content_center, common_event, NULL, NULL },
	{ PAPERMONO_APP_REMOTE, "遥控器", true, NULL, render_remote, remote_event, remote_close, &remote_state },
	{ PAPERMONO_APP_CALENDAR, "日历", true, organizer_open, render_calendar, calendar_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_TASKS, "待办", true, organizer_open, render_tasks, tasks_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_NOTES, "便签", true, organizer_open, render_notes, notes_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_INFO, "信息卡片", true, NULL, render_info, info_event, NULL, &organizer_ui },
	{ PAPERMONO_APP_TOOLS, "硬件工具", true, NULL, render_tools, tools_event, NULL, NULL },
	{ PAPERMONO_APP_POWER, "电源", true, NULL, render_power, power_event, power_close, NULL },
	{ PAPERMONO_APP_SETTINGS, "设置", true, NULL, render_settings, settings_event, NULL, NULL },
	{ PAPERMONO_APP_SYSTEM, "系统状态", true, NULL, render_system, system_event, NULL, NULL },
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
		.queued_at_ms = k_uptime_get_32(),
	};

	return k_msgq_put(&ui_events, &event, K_NO_WAIT);
}

#if defined(CONFIG_PAPERMONO_BLE_MIC)
/* Ask for a voice page redraw from the input callback or the microphone worker.
 *
 * Route both sources through the background-refresh pending flag: a press and
 * the worker's STARTING/STREAMING transition then coalesce into one render,
 * and a release plus the worker's stop coalesce into another. It also keeps
 * the request deferred while a panel refresh is in progress instead of
 * queueing a back-to-back refresh from the touch thread. The event is
 * system-sourced, so the Wi-Fi keyboard generation guard does not apply.
 */
static void queue_voice_refresh(void)
{
	papermono_ui_request_background_refresh();
}
#endif

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

static int submit_input_event(const struct papermono_app_event *completed)
{
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	/* Character taps must remain FIFO, not the single deferred navigation
	 * slot. Saturation is explicit and disables connect until re-entered. */
	if (atomic_get(&wifi_layout_transition) && papermono_app_event_is_touch(completed)) {
		return -EAGAIN;
	}
	if (atomic_get(&wifi_keyboard_active)) {
		int ret = k_msgq_put(&ui_events, completed, K_NO_WAIT);
		if (ret < 0) atomic_set(&wifi_keyboard_overflow, 1);
		return ret;
	}
#endif
	if (atomic_get(&input_blocked) != 0) {
		k_spinlock_key_t key = k_spin_lock(&deferred_touch_lock);
		const bool still_blocked = atomic_get(&input_blocked) != 0;
		bool retained = false;

		if (still_blocked) {
			retained = papermono_interaction_defer_event(
				&deferred_touch_event, &deferred_touch_valid, completed);
		}
		k_spin_unlock(&deferred_touch_lock, key);
		if (still_blocked) {
			LOG_INF("UI input type=%d %s during refresh",
				completed->type, retained ? "retained" : "coalesced");
			return 0;
		}
	}
	return k_msgq_put(&ui_events, completed, K_NO_WAIT);
}

static int submit_touch_gesture(enum papermono_app_event_type type,
	uint32_t duration_ms, uint32_t now)
{
	struct papermono_app_event completed = {
		.type = type,
		.source = PAPERMONO_APP_EVENT_SOURCE_TOUCH,
		.x = touch_start_x,
		.y = touch_start_y,
		.duration_ms = duration_ms,
		.queued_at_ms = now,
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		.input_generation = touch_input_generation,
#endif
	};
	const int ret = submit_input_event(&completed);

	LOG_DBG("UI touch gesture type=%d duration=%u queue=%d",
		type, duration_ms, ret);
	if (ret == 0) {
		last_touch_action_ms = now;
	}
	return ret;
}

static void papermono_ui_input_event(struct input_event *event,
	void *user_data)
{
	ARG_UNUSED(user_data);
	if (event->dev == pmic && event->type == INPUT_EV_KEY &&
	    event->code == INPUT_KEY_POWER && event->value != 0) {
		if (atomic_get(&power_input_ready) == 0) {
			LOG_INF("UI power click ignored during startup");
			return;
		}
		const struct papermono_app_event completed = {
			.type = PAPERMONO_APP_EVENT_SLEEP,
			.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON,
			.x = -1, .y = -1,
			.queued_at_ms = k_uptime_get_32(),
		};
		const int ret = submit_input_event(&completed);

		LOG_INF("UI power click queued (%d)", ret);
		return;
	}
	if (event->dev == buttons && event->type == INPUT_EV_KEY &&
	    event->value != 0) {
		if (event->code == INPUT_KEY_0 || event->code == INPUT_KEY_1) {
			const struct papermono_app_event completed = {
				.type = event->code == INPUT_KEY_0 ?
					PAPERMONO_APP_EVENT_PREVIOUS : PAPERMONO_APP_EVENT_NEXT,
				.source = PAPERMONO_APP_EVENT_SOURCE_BUTTON,
				.x = -1, .y = -1,
				.queued_at_ms = k_uptime_get_32(),
			};

			(void)submit_input_event(&completed);
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
#if defined(CONFIG_PAPERMONO_BLE_MIC)
		/* A hold-to-talk contact owns the whole gesture: it must not move the
		 * pointer or commit edge navigation back/home while it is active.
		 */
		if (voice_hold) {
			return;
		}
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
		if (touch_pressed && touchpad_captured) {
			if ((uint32_t)atomic_get(&remote_touchpad_epoch) !=
			    touchpad_contact_epoch) {
				touchpad_captured = false;
				touch_navigation_sent = true;
				return;
			}
			int16_t dx, dy, wheel;
			if (papermono_touchpad_update(&touchpad_contact,
				touch_x, touch_y, k_uptime_get_32(), &dx, &dy, &wheel)) {
				papermono_hid_mouse_move(dx, dy, wheel);
				atomic_set(&remote_touchpad_activity, k_uptime_get_32());
			}
			return;
		}
#endif
		/* FT6336 reports X then Y for each complete controller sample. Only
		 * inspect the pair here, never an X value with the previous frame's Y.
		 */
		if (touch_pressed && !touch_navigation_sent) {
			const uint32_t now = k_uptime_get_32();
			const uint32_t duration = now - touch_start_ms;
			enum papermono_app_event_type type;

			if (papermono_touch_gesture_navigation(touch_start_x,
				touch_start_y, touch_x, touch_y, duration,
				UI_WIDTH, UI_HEIGHT, &type) == 0 &&
			    submit_touch_gesture(type, duration, now) == 0) {
				touch_navigation_sent = true;
				LOG_INF("UI edge navigation committed before release");
			}
		}
	} else if (event->type == INPUT_EV_KEY &&
		   event->code == INPUT_BTN_TOUCH) {
		if (event->value != 0) {
			touch_pressed = true;
			touch_navigation_sent = false;
			touch_start_x = touch_x;
			touch_start_y = touch_y;
			touch_start_ms = k_uptime_get_32();
#if defined(CONFIG_PAPERMONO_BLE_MIC)
			/* Capture the voice epoch the same way a touchpad contact does:
			 * the page publishes availability only while it is the one being
			 * drawn, so a press captured here cannot start a stream from
			 * another page or after the subscription went away. A press that
			 * lands during a panel refresh must not start a session: the
			 * refresh guard has not classified it yet and starting here would
			 * bypass the deferral.
			 */
			voice_hold = atomic_get(&input_blocked) == 0 &&
				((uint32_t)atomic_get(&remote_voice_epoch) & 1U) != 0U &&
				touch_x >= remote_voice_pad.x &&
				touch_x < remote_voice_pad.x + remote_voice_pad.width &&
				touch_y >= remote_voice_pad.y &&
				touch_y < remote_voice_pad.y + remote_voice_pad.height;
			if (voice_hold) {
				const int start_ret = papermono_ble_mic_request_start();

				if (start_ret < 0) {
					/* Publish the failure for the next render and leave the
					 * contact unpublished, so a release cannot stop a stream
					 * that was never requested.
					 */
					atomic_set(&remote_voice_start_error, start_ret);
				} else {
					atomic_set(&remote_voice_start_error, 0);
					atomic_set(&remote_voice_contact, 1);
				}
				/* Never wait for the worker here; the page redraws from the
				 * state it publishes.
				 */
				queue_voice_refresh();
			}
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
			touchpad_contact_epoch = (uint32_t)atomic_get(&remote_touchpad_epoch);
			/* This epoch is published only after the mouse page is visible,
			 * and revoked before leaving it. A same-page EPD refresh must not
			 * suppress new touchpad contacts through the generic input guard.
			 */
			touchpad_captured = (touchpad_contact_epoch & 1U) != 0U &&
				touch_x >= MAX(remote_mouse_pad.x, UI_WIDTH / 10) &&
				touch_x < remote_mouse_pad.x + remote_mouse_pad.width &&
				touch_y >= remote_mouse_pad.y &&
				touch_y < remote_mouse_pad.y + remote_mouse_pad.height;
			if (touchpad_captured) {
				papermono_touchpad_begin(&touchpad_contact,
					touch_x, touch_y, touch_start_ms, touch_x >= 360);
			}
#endif
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
			touch_input_generation = atomic_get(&wifi_layout_transition) ? UINT32_MAX :
				(uint32_t)atomic_get(&wifi_input_generation);
#endif
			LOG_DBG("UI touch press blocked=%d", (int)atomic_get(&input_blocked));
		} else if (touch_pressed) {
			const uint32_t now = k_uptime_get_32();
			const uint32_t duration = now - touch_start_ms;
			enum papermono_app_event_type type;
			int ret;

			touch_pressed = false;
#if defined(CONFIG_PAPERMONO_BLE_MIC)
			if (voice_hold) {
				voice_hold = false;
				if (atomic_cas(&remote_voice_contact, 1, 0)) {
					(void)papermono_ble_mic_request_stop();
				}
				queue_voice_refresh();
				/* This contact was a hold, not a tap, long press, swipe or
				 * edge gesture, so classification must not run for it.
				 */
				return;
			}
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
			if (touchpad_captured) {
				touchpad_captured = false;
				if (touchpad_contact.moved || touchpad_contact_epoch !=
				    (uint32_t)atomic_get(&remote_touchpad_epoch)) {
					touch_navigation_sent = false;
					return;
				}
			}
#endif
			if (touch_navigation_sent) {
				touch_navigation_sent = false;
				return;
			}
			ret = papermono_touch_gesture_classify(touch_start_x,
				touch_start_y, touch_x, touch_y, duration,
				UI_WIDTH, UI_HEIGHT, &type);
			if (ret < 0) {
				LOG_DBG("UI touch gesture rejected duration=%u (%d)", duration, ret);
				return;
			}
			/* Controller bounce is filtered for taps only. A deliberate edge
			 * return must never be rejected because it follows a recent tap.
			 */
			if (type == PAPERMONO_APP_EVENT_TAP &&
			    now - last_touch_action_ms < UI_TOUCH_ACTION_COOLDOWN_MS) {
				return;
			}
			(void)submit_touch_gesture(type, duration, now);
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
	 *
	 * The deferred event must be queued before input_blocked is cleared, and
	 * both must happen under one lock hold. The FT6336 thread runs at a higher
	 * priority than this one, so releasing the lock between the clear and the
	 * k_msgq_put() let it observe input_blocked == 0, fall through to its own
	 * k_msgq_put(), and land a *newer* event ahead of the stale one. The stale
	 * event was then routed against the screen the newer event had opened,
	 * which is how a retained tap on one page could activate a control on the
	 * next one and run a destructive action without its confirmation.
	 */
	k_spinlock_key_t key = k_spin_lock(&deferred_touch_lock);
	int restore_ret = 0;

	if (deferred_touch_valid) {
		restore_ret = k_msgq_put(&ui_events, &deferred_touch_event,
			K_NO_WAIT);
		if (restore_ret == 0) {
			deferred_touch_valid = false;
		}
		/* On a full queue the event stays in the slot and is retried on
		 * the next release, instead of being dropped.
		 */
	}
	/* k_msgq_put() with K_NO_WAIT never blocks; on a full queue it returns
	 * -ENOMSG after taking only the queue's own spinlock.
	 */
	atomic_clear(&input_blocked);
	k_spin_unlock(&deferred_touch_lock, key);
	if (restore_ret < 0) {
		LOG_WRN("Deferred input kept for retry; queue full (%d)",
			restore_ret);
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
	if (app_router.current != PAPERMONO_APP_REMOTE ||
	    remote_state.mode != REMOTE_MODE_MOUSE) {
		remote_touchpad_enable(false);
	}
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	if (app_router.current != PAPERMONO_APP_REMOTE ||
	    remote_state.mode != REMOTE_MODE_VOICE) {
		/* Every route off the voice page passes through here, including the
		 * ones that do not run remote_close() first.
		 */
		remote_voice_set_armed(false);
	}
#endif

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
#if defined(CONFIG_PAPERMONO_BLE_HID)
	struct papermono_hid_state hid;
	papermono_hid_get_state(&hid);
	remote_touchpad_enable(ret == 0 && app_router.current == PAPERMONO_APP_REMOTE &&
		remote_state.mode == REMOTE_MODE_MOUSE && remote_ready(&hid));
#endif
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	if (ret == 0) atomic_set(&wifi_layout_transition, 0);
#endif
	last_refresh_ms = k_uptime_get();
	return ret;
}

static int open_app(enum papermono_app_id target)
{
	remote_touchpad_enable(false);
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	wifi_advance_input();
#endif
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


/* Standby page: a static plate drawn once before deep sleep. It deliberately
 * shows no clock - the image stays frozen, so a time would go stale instantly.
 * Every element sits on the 22/458 margin grid used by the rest of the UI.
 */
static lv_obj_t *sleep_label(lv_obj_t *parent, const char *text,
	const lv_font_t *font, int32_t x, int32_t y, int32_t width,
	bool right_aligned)
{
	lv_obj_t *label = make_label(parent, text, font, ink);

	lv_obj_set_pos(label, x, y);
	lv_obj_set_size(label, width, 24);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	if (right_aligned) {
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	}
	return label;
}

static void render_sleep_screen(lv_obj_t *screen, bool shutdown)
{
	struct papermono_ui_snapshot snapshot;

	read_snapshot(&snapshot);
	(void)sleep_label(screen, "PAPERMONO", &lv_font_montserrat_18, 22, 16,
		200, false);
	if (snapshot.rtc_valid) {
		char date[48];

		(void)snprintf(date, sizeof(date), "休眠于 %02d月%02d日",
			snapshot.time.tm_mon + 1, snapshot.time.tm_mday);
		(void)sleep_label(screen, date, &papermono_cjk_16, 258, 16, 200, true);
	} else {
		(void)sleep_label(screen, "时钟尚未校准", &papermono_cjk_16, 258, 16,
			200, true);
	}
	(void)make_ink_shape(screen, 22, 116, 436, 1, ink, 0);

	if (snapshot.rtc_valid) {
		static const char *const weekday[] = {
			"周日", "周一", "周二", "周三", "周四", "周五", "周六",
		};
		char text[32];
		lv_obj_t *day;

		(void)snprintf(text, sizeof(text), "%02d", snapshot.time.tm_mday);
		day = make_label(screen, text, &papermono_clock_148, ink);
		lv_obj_set_pos(day, 14, 126);
		(void)snprintf(text, sizeof(text), "%d月", snapshot.time.tm_mon + 1);
		(void)sleep_label(screen, text, &papermono_cjk_16, 196, 138, 130, false);
		(void)sleep_label(screen, weekday[CLAMP(snapshot.time.tm_wday, 0, 6)],
			&papermono_reading_24, 196, 168, 150, false);

		struct papermono_almanac_day almanac;

		if (papermono_almanac_lookup(snapshot.time.tm_year + 1900,
			snapshot.time.tm_mon + 1, snapshot.time.tm_mday,
			&almanac) == 0 && almanac.term_valid) {
			(void)sleep_label(screen, almanac.term.name, &papermono_cjk_16,
				352, 136, 100, false);
			if (almanac.term.days_away == 0) {
				(void)sleep_label(screen, "今日", &papermono_cjk_16, 352,
					158, 100, false);
			} else {
				(void)snprintf(text, sizeof(text), "%d 天后",
					almanac.term.days_away);
				(void)sleep_label(screen, text, &papermono_cjk_16, 352,
					158, 100, false);
			}
			if (almanac.memorial != NULL) {
				(void)make_ink_shape(screen, 352, 182, 90, 1, ink, 0);
				(void)sleep_label(screen, almanac.memorial,
					&papermono_cjk_16, 352, 192, 104, false);
			}
		}
	} else {
		(void)sleep_label(screen, "日期未知", &papermono_reading_24, 22, 150,
			300, false);
	}
	(void)make_ink_shape(screen, 22, 316, 436, 1, ink, 0);
	(void)sleep_label(screen, "天气未同步 · 唤醒后更新", &papermono_cjk_16, 22,
		330, 400, false);

	if (shutdown) {
		(void)sleep_label(screen, "已关机 · 按电源键重新启动",
			&papermono_reading_24, 22, 652, 420, false);
		return;
	}
	const struct papermono_preferences *prefs = papermono_settings_get();

	if (prefs->sleep_summary == PAPERMONO_SLEEP_SUMMARY_QUOTE &&
	    snapshot.rtc_valid) {
		const struct papermono_quote *quote = papermono_quote_for_day(
			snapshot.time.tm_year + 1900, snapshot.time.tm_mon + 1,
			snapshot.time.tm_mday);
		char source[64];

		(void)sleep_label(screen, "摘  抄", &papermono_cjk_16, 22, 436, 120,
			false);
		(void)sleep_label(screen, quote->text, &papermono_reading_24, 22,
			462, 436, false);
		(void)snprintf(source, sizeof(source), "—《%s》", quote->book);
		(void)sleep_label(screen, source, &papermono_cjk_16, 258, 548, 200,
			true);
	}
	(void)make_ink_shape(screen, 22, 640, 436, 1, ink, 0);
	(void)sleep_label(screen, "下一日程", &papermono_cjk_16, 24, 652, 100,
		false);
	(void)sleep_label(screen, "唤醒后查看", &papermono_reading_24, 152, 648,
		300, false);
	(void)make_ink_shape(screen, 22, 700, 436, 1, ink, 0);
	(void)sleep_label(screen, "轻触屏幕或按电源键唤醒", &papermono_cjk_16, 22,
		704, 436, false);
}

static int enter_power_state(bool shutdown)
{
	lv_obj_t *screen = lv_screen_active();
	int ret;
	remote_touchpad_enable(false);
#if defined(CONFIG_PAPERMONO_BLE_MIC)
	/* Sleep and shutdown reach here from both the power click and the idle
	 * timeout. Release the hold first, then let the worker power the PDM
	 * microphone down before the panel and radio stop.
	 */
	remote_voice_set_armed(false);
	papermono_ble_mic_abort();
#endif

	confirmation = CONFIRM_NONE;
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	wifi_input_close();
#endif
	if (!shutdown) {
		ret = papermono_power_input_wake_ready(1000U);
		if (ret < 0) {
			power_last_error = ret;
			LOG_WRN("Wake inputs are not idle; sleep cancelled (%d)",
				ret);
			return render_current(PAPERMONO_REFRESH_AUTO);
		}
	}
	power_last_error = 0;
	/* Drop the page-global label handles before their objects are deleted.
	 * papermono_ui_demo_update() dereferences clock_label/footer_label without
	 * checking that the screen still owns them, so leaving them dangling here
	 * would write through freed lv_obj_t pointers on the next demo update.
	 */
	clock_label = NULL;
	footer_label = NULL;
	lv_obj_clean(screen);
	ui_fixed_line_count = 0U;
	set_box_style(screen, paper, 0, 0);
	render_sleep_screen(screen, shutdown);
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

/* Both the HTTP content centre and the authenticated BLE sync can be holding
 * acknowledged transfer state. Sleeping mid-transfer truncates it, so either
 * source defers automatic sleep and a power click.
 */
static bool transfer_in_progress(void)
{
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	if (papermono_content_server_busy()) {
		return true;
	}
#endif
#if defined(CONFIG_PAPERMONO_BLE_SYNC)
	struct papermono_sync_state sync;

	papermono_sync_get_state(&sync);
	if (sync.active) {
		return true;
	}
#endif
	return false;
}

static int __maybe_unused process_event(
	const struct papermono_app_event *event)
{
	struct papermono_app_command command;
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	if (papermono_app_event_is_touch(event) &&
	    event->input_generation != (uint32_t)atomic_get(&wifi_input_generation)) {
		return 0;
	}
#endif
	const bool background_refresh = event->type == PAPERMONO_APP_EVENT_REFRESH ||
		event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED;
	if (event->type == PAPERMONO_APP_EVENT_CONTENT_CHANGED) {
		(void)papermono_organizer_reload();
	}
	int ret;

	if (event->type == PAPERMONO_APP_EVENT_SLEEP &&
	    event->source == PAPERMONO_APP_EVENT_SOURCE_BUTTON) {
		if (transfer_in_progress()) {
			LOG_WRN("Power click sleep cancelled during an active transfer");
			return 0;
		}
		LOG_INF("UI power click entering deep sleep from app=%d",
			app_router.current);
		return enter_power_state(false);
	}
	if (event->type == PAPERMONO_APP_EVENT_HOME) {
		remote_touchpad_enable(false);
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		wifi_advance_input();
#endif
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
		remote_touchpad_enable(false);
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		wifi_advance_input();
#endif
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
	/* An idle provisioning AP must still obey the configured sleep timeout.
	 * Only defer sleep while a bounded request or sync owns transfer state;
	 * waking the device brings the AP back for later provisioning.
	 */
	return transfer_in_progress();
}

int papermono_ui_init(const struct device *display)
{
	enum papermono_app_id initial = PAPERMONO_APP_HOME;
	int ret;

	if (display == NULL || !device_is_ready(display)) {
		return -ENODEV;
	}
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
#if defined(CONFIG_PAPERMONO_FT6336)
	BUILD_ASSERT(CONFIG_PAPERMONO_UI_THREAD_PRIORITY >
		CONFIG_PAPERMONO_FT6336_THREAD_PRIORITY,
		"Touch polling must preempt LVGL layout");
#endif
	k_thread_priority_set(k_current_get(), CONFIG_PAPERMONO_UI_THREAD_PRIORITY);
#endif
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
	if (clock_label == NULL) {
		return -EFAULT;
	}
	lv_label_set_text_fmt(clock_label, "10:%02u", (28U + update) % 60U);
	if (footer_label != NULL) {
		lv_label_set_text_fmt(footer_label, "PARTIAL %02u / %02u", update, total);
	}
	return papermono_display_refresh(PAPERMONO_REFRESH_FAST);
}

int papermono_ui_run(void)
{
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	atomic_set(&power_input_ready, 1);
	LOG_INF("UI power input ready: short click sleeps");
	for (;;) {
		struct papermono_app_event event;
		const struct papermono_preferences *prefs = papermono_settings_get();
		const int64_t now = k_uptime_get();
		int64_t deadline = INT64_MAX;
		int ret;
		update_touchpad_activity(now);

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
				LOG_DBG("UI processing touch type=%d duration=%u app=%d wait=%u ms",
					event.type, event.duration_ms,
					app_router.current,
					(uint32_t)(k_uptime_get_32() - event.queued_at_ms));
			}
			const uint32_t action_started = k_uptime_get_32();

			ret = process_event(&event);
			if (papermono_app_event_is_touch(&event) ||
			    papermono_app_event_is_button(&event)) {
				LOG_INF("UI action complete app=%d elapsed=%u ms result=%d",
					app_router.current,
					(uint32_t)(k_uptime_get_32() - action_started), ret);
			}
		} else if (ret == -EAGAIN) {
			const int64_t current = k_uptime_get();
			/* Raw movement bypasses the UI queue. Recheck its timestamp after
			 * a timeout instead of sleeping against the old idle deadline.
			 */
			update_touchpad_activity(current);

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
			}
#if CONFIG_PAPERMONO_UI_AUTO_REFRESH_MINUTES > 0
			else if (current - last_refresh_ms >=
				 (int64_t)CONFIG_PAPERMONO_UI_AUTO_REFRESH_MINUTES * 60000) {
				ret = render_current(PAPERMONO_REFRESH_AUTO);
			}
#endif
			else {
				ret = 0;
			}
		}
		if (ret < 0) {
			papermono_status_led_report_runtime_fault(true);
			LOG_ERR("UI action failed (%d); keeping the event loop alive", ret);
			if (ui_failure_streak < UINT8_MAX) {
				++ui_failure_streak;
			}
			/* One full-panel recovery redraw is worth attempting, but a
			 * persistently failing action must not keep driving the
			 * panel: a full refresh blocks for seconds, costs power and
			 * adds ghosting, so repeating it on every event is worse
			 * than leaving the last good frame on screen. After the
			 * first attempt, back off and wait for the next event.
			 */
			if (ui_failure_streak <= UI_FAILURE_RECOVERY_ATTEMPTS) {
				k_msleep(100);
				ret = render_current(PAPERMONO_REFRESH_FULL);
				if (ret < 0) {
					LOG_ERR("UI recovery refresh failed (%d); waiting for the next event",
						ret);
				} else {
					papermono_status_led_report_runtime_fault(false);
				}
			} else {
				LOG_WRN("UI action failed %u times in a row; "
					"skipping further recovery redraws",
					ui_failure_streak);
				k_msleep(1000);
			}
		} else {
			ui_failure_streak = 0U;
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
