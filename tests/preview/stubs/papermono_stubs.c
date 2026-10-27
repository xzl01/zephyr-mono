/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Canned stand-ins for the PAPER MONO subsystems that cannot be ported to the
 * host: anything that needs BLE, Wi-Fi, I2C, PWM or the SSD1677 panel driver.
 *
 * Screenshot fixtures return fixed, DOCUMENTED demo state; Wi-Fi interaction
 * tests can inject synthetic state/results and count API calls. Nothing here
 * pretends to be measured data: the values are the demo fixture described in
 * the task ("2026-09-30 09:41", "3980 mV battery", "idle/ready sync + HID").
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>

#include <papermono/weather.h>
#include <papermono/rss.h>

#include <papermono/content_server.h>
#include <papermono/display_scheduler.h>
#include <papermono/frontlight.h>
#include <papermono/ft6336.h>
#include <papermono/hid.h>
#include <papermono/m5pm1.h>
#include <papermono/power.h>
#include <papermono/status_led.h>
#include <papermono/sync.h>
#include <papermono/zip_reader.h>

/* --------------------------------------------------------------- m5pm1 PMIC */

int m5pm1_read_voltage(const struct device *dev, enum m5pm1_voltage source,
	uint16_t *millivolts)
{
	(void)dev;
	if (millivolts == NULL) {
		return -EINVAL;
	}
	/* Demo fixture: 3.980 V pack, 5.010 V input, 0 V on the 5 V rail. */
	switch (source) {
	case M5PM1_VOLTAGE_BATTERY:
		*millivolts = 3980U;
		break;
	case M5PM1_VOLTAGE_INPUT:
		*millivolts = 5010U;
		break;
	case M5PM1_VOLTAGE_5V_OUTPUT:
		*millivolts = 0U;
		break;
	default:
		*millivolts = 0U;
		break;
	}
	return 0;
}

int m5pm1_get_power_source(const struct device *dev, uint8_t *source)
{
	(void)dev;
	if (source == NULL) {
		return -EINVAL;
	}
	*source = 0U; /* battery */
	return 0;
}

int m5pm1_get_info(const struct device *dev, struct m5pm1_info *info)
{
	(void)dev;
	if (info == NULL) {
		return -EINVAL;
	}
	memset(info, 0, sizeof(*info));
	return 0;
}

int m5pm1_get_boot_wake_source(const struct device *dev, uint8_t *source)
{
	(void)dev;
	if (source == NULL) {
		return -EINVAL;
	}
	*source = 0U;
	return 0;
}

/* ----------------------------------------------------------------- sensor */

int sensor_sample_fetch(const struct device *dev)
{
	(void)dev;
	return 0;
}

int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
	struct sensor_value *val)
{
	(void)dev;
	(void)chan;
	if (val == NULL) {
		return -EINVAL;
	}
	val->val1 = 0;
	val->val2 = 0;
	return 0;
}

/* ------------------------------------------------------- display scheduling */

static uint32_t refresh_count;
static int preview_refresh_result;

void preview_display_result(int result)
{
	preview_refresh_result = result;
}

int papermono_display_scheduler_init(const struct device *display,
	papermono_display_input_guard_t input_guard, void *context)
{
	(void)display;
	(void)input_guard;
	(void)context;
	return 0;
}

int papermono_display_refresh(enum papermono_refresh_quality quality)
{
	/* The preview harness drives refresh itself; this only records the call so
	 * the UI's own bookkeeping stays consistent.
	 */
	(void)quality;
	++refresh_count;
	return preview_refresh_result;
}

struct device papermono_preview_device_slots[PAPERMONO_PREVIEW_DEVICE_SLOTS] = {
	{ .name = "rtc" },
	{ .name = "m5pm1" },
	{ .name = "touch" },
	{ .name = "buttons" },
	{ .name = "imu" },
	{ .name = "nfc" },
	{ .name = "lora" },
	{ .name = "microphone" },
	{ .name = "display" },
};

int display_set_brightness(const struct device *dev, uint8_t brightness)
{
	/* The preview has no frontlight hardware; report success so the UI keeps
	 * its normal "frontlight applied" state.
	 */
	(void)dev;
	(void)brightness;
	return 0;
}

int display_blanking_on(const struct device *dev)
{
	(void)dev;
	return 0;
}

int display_blanking_off(const struct device *dev)
{
	(void)dev;
	return 0;
}

int display_write(const struct device *dev, uint16_t x, uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf)
{
	(void)dev;
	(void)x;
	(void)y;
	(void)desc;
	(void)buf;
	return 0;
}

int display_get_capabilities(const struct device *dev,
	struct display_capabilities *caps)
{
	(void)dev;
	if (caps == NULL) {
		return -EINVAL;
	}
	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = 480U;
	caps->y_resolution = 800U;
	return 0;
}

/* ------------------------------------------------------------- frontlight */

int papermono_frontlight_output_level(uint8_t percent, uint8_t *level)
{
	if (level == NULL) {
		return -EINVAL;
	}
	*level = percent;
	return 0;
}

uint8_t papermono_frontlight_next(uint8_t percent)
{
	static const uint8_t steps[] = { 0U, 10U, 25U, 50U, 75U, 100U };

	for (size_t index = 0U; index < sizeof(steps); ++index) {
		if (percent < steps[index]) {
			return steps[index];
		}
	}
	return 100U;
}

uint8_t papermono_frontlight_previous(uint8_t percent)
{
	static const uint8_t steps[] = { 0U, 10U, 25U, 50U, 75U, 100U };

	for (size_t index = sizeof(steps); index > 0U; --index) {
		if (percent > steps[index - 1U]) {
			return steps[index - 1U];
		}
	}
	return 0U;
}

/* ------------------------------------------------------------ touch panel */

int papermono_ft6336_get_status(struct papermono_ft6336_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}
	memset(status, 0, sizeof(*status));
	status->online = true;
	status->pressed = false;
	status->frames = 12000U;
	status->press_events = 42U;
	status->release_events = 42U;
	status->x = 240;
	status->y = 400;
	return 0;
}

/* ------------------------------------------------------------------- EPUB */

/* The preview ships only plain-text books, so the miniz-backed ZIP reader is
 * not built.  Every entry point reports "unsupported" instead of pretending an
 * .epub was parsed.
 */
int papermono_zip_open(struct papermono_zip_archive *archive,
	const struct papermono_content_entry *source)
{
	(void)archive;
	(void)source;
	return -ENOTSUP;
}

int papermono_zip_find(const struct papermono_zip_archive *archive,
	const char *name, struct papermono_zip_entry *entry)
{
	(void)archive;
	(void)name;
	(void)entry;
	return -ENOTSUP;
}

int papermono_zip_extract(const struct papermono_zip_archive *archive,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context)
{
	(void)archive;
	(void)entry;
	(void)callback;
	(void)context;
	return -ENOTSUP;
}

/* ------------------------------------------------------------ BLE HID link */

int papermono_hid_send_action(const struct papermono_hid_action *action)
{
	(void)action;
	return 0;
}

int papermono_hid_clear_bonds(void)
{
	return 0;
}

int papermono_hid_set_battery_level(uint8_t percent)
{
	(void)percent;
	return 0;
}

static struct papermono_hid_state preview_hid_state = {
	.initialized = true,
	.secured = true,
	.bonded = true,
};

void preview_hid_set_state(const struct papermono_hid_state *state)
{
	preview_hid_state = *state;
}

void papermono_hid_get_state(struct papermono_hid_state *state)
{
	if (state == NULL) {
		return;
	}
	/* Demo fixture: link idle but ready (host bonded, not currently driving). */
	*state = preview_hid_state;
}

/* ------------------------------------------------------------ Wi-Fi / sync */

/* Synthetic snapshots only: never scan a host radio or persist credentials. */
static struct papermono_wifi_state preview_wifi;

/* Test-only call/result injection. Counters distinguish API attempts from
 * accepted synthetic requests; none of these execute a radio/NVS worker.
 * Never retain a password copy: only match against a caller-owned demo value. */
enum preview_wifi_operation {
	PREVIEW_WIFI_SCAN, PREVIEW_WIFI_SAVED, PREVIEW_WIFI_FORGET,
	PREVIEW_WIFI_CONNECT, PREVIEW_WIFI_RESET, PREVIEW_WIFI_OPERATIONS
};
static struct {
	unsigned calls[PREVIEW_WIFI_OPERATIONS];
	unsigned accepted[PREVIEW_WIFI_OPERATIONS];
	int result[PREVIEW_WIFI_OPERATIONS];
	const struct papermono_wifi_network *expected_network;
	const char *expected_password;
	bool connect_matched;
} preview_wifi_probe;

void preview_wifi_probe_reset(void)
{
	memset(&preview_wifi_probe, 0, sizeof(preview_wifi_probe));
}

void preview_wifi_result(unsigned operation, int result)
{
	if (operation < PREVIEW_WIFI_OPERATIONS)
		preview_wifi_probe.result[operation] = result;
}

unsigned preview_wifi_calls(unsigned operation, bool accepted)
{
	if (operation >= PREVIEW_WIFI_OPERATIONS) return 0;
	return accepted ? preview_wifi_probe.accepted[operation] :
		preview_wifi_probe.calls[operation];
}

void preview_wifi_expect_connect(const struct papermono_wifi_network *network,
	const char *password)
{
	preview_wifi_probe.expected_network = network;
	preview_wifi_probe.expected_password = password;
	preview_wifi_probe.connect_matched = false;
}

bool preview_wifi_connect_matched(void)
{
	return preview_wifi_probe.connect_matched;
}

void preview_wifi_set_state(const struct papermono_wifi_state *state)
{
	if (state != NULL) preview_wifi = *state;
}

static int preview_wifi_request(unsigned operation)
{
	++preview_wifi_probe.calls[operation];
	int result = preview_wifi_probe.result[operation];
	if (result == 0) ++preview_wifi_probe.accepted[operation];
	return result;
}

int preview_wifi_fixture(const char *name)
{
	memset(&preview_wifi, 0, sizeof(preview_wifi));
	preview_wifi.configured = true;
	strcpy(preview_wifi.saved_ssid, "Field Notes 2.4G");
	preview_wifi.connection = PAPERMONO_WIFI_CONNECTED;
	preview_wifi.scan_generation = 1;
	preview_wifi.count = 6;
	static const char *const ssids[] = {
		"Field Notes 2.4G", "Guest Open", "Office WPA3", "",
		"Long-Network-SSID-1234567890123456", "Studio WPA2"
	};
	for (size_t i = 0; i < preview_wifi.count; ++i) {
		struct papermono_wifi_network *n = &preview_wifi.networks[i];
		snprintf(n->ssid, sizeof(n->ssid), "%s", ssids[i]);
		n->ssid_length = strlen(n->ssid);
		n->rssi = -38 - (int)i * 8;
		n->channel = 1 + (i % 3) * 5;
		n->security = i == 1 ? PAPERMONO_WIFI_SECURITY_OPEN :
			i == 2 ? PAPERMONO_WIFI_SECURITY_UNSUPPORTED : PAPERMONO_WIFI_SECURITY_WPA2_PSK;
		n->hidden = i == 3;
		n->supported = i != 2 && i != 3;
	}
	if (strcmp(name, "wifi-scan") == 0) {
		preview_wifi.scan_busy = true;
		preview_wifi.count = 0;
		preview_wifi.configured = false;
		preview_wifi.connection = PAPERMONO_WIFI_DISCONNECTED;
	} else if (strcmp(name, "wifi-connecting") == 0) {
		preview_wifi.connection = PAPERMONO_WIFI_CONNECTING;
		preview_wifi.request_busy = true;
	} else if (strcmp(name, "wifi-failure") == 0) {
		preview_wifi.connection = PAPERMONO_WIFI_CONNECTION_ERROR;
		preview_wifi.last_connection_error = -ETIMEDOUT;
	} else if (strcmp(name, "wifi-scan-failure") == 0) {
		preview_wifi.scan_error = -EIO;
		preview_wifi.count = 0;
	} else if (strcmp(name, "wifi-empty") == 0) {
		preview_wifi.count = 0;
	}
	return 0;
}

void papermono_content_server_get_wifi_state(struct papermono_wifi_state *state)
{
	if (state != NULL) *state = preview_wifi;
}

int papermono_content_server_scan_wifi(void)
{
	return preview_wifi_request(PREVIEW_WIFI_SCAN);
}
int papermono_content_server_connect_saved(void)
{
	return preview_wifi_request(PREVIEW_WIFI_SAVED);
}
int papermono_content_server_forget_wifi(void)
{
	return preview_wifi_request(PREVIEW_WIFI_FORGET);
}
int papermono_content_server_connect_wifi(const struct papermono_wifi_network *network,
	const char *password)
{
	const struct papermono_wifi_network *expected = preview_wifi_probe.expected_network;
	preview_wifi_probe.connect_matched = expected != NULL && network != NULL &&
		preview_wifi_probe.expected_password != NULL && password != NULL &&
		network->ssid_length == expected->ssid_length &&
		network->security == expected->security &&
		strcmp(network->ssid, expected->ssid) == 0 &&
		strcmp(password, preview_wifi_probe.expected_password) == 0;
	return preview_wifi_request(PREVIEW_WIFI_CONNECT);
}

void papermono_content_server_get_state(
	struct papermono_content_server_state *state)
{
	if (state == NULL) {
		return;
	}
	memset(state, 0, sizeof(*state));
	/* Demo fixture only, consistent with the selected synthetic Wi-Fi state. */
	state->station_connected = preview_wifi.connection == PAPERMONO_WIFI_CONNECTED;
	state->station_connecting = preview_wifi.connection == PAPERMONO_WIFI_CONNECTING;
	if (state->station_connected)
		snprintf(state->station_ipv4, sizeof(state->station_ipv4), "192.168.1.42");
	state->client_paired = true;
	state->content_generation = 1U;
}

bool papermono_content_server_busy(void)
{
	return false;
}

int papermono_content_server_reset_wifi(void)
{
	return preview_wifi_request(PREVIEW_WIFI_RESET);
}

bool preview_sync_active;

void papermono_sync_get_state(struct papermono_sync_state *state)
{
	if (state == NULL) {
		return;
	}
	memset(state, 0, sizeof(*state));
	state->initialized = true;
	state->active = preview_sync_active;
	state->last_error = 0;
}

/* ------------------------------------------------------------- status LED */

void papermono_status_led_request_refresh(void)
{
}

void papermono_status_led_resume(void)
{
}

void papermono_status_led_report_runtime_fault(bool fault)
{
	(void)fault;
}

void papermono_status_led_report_startup_fault(bool fault)
{
	(void)fault;
}

void papermono_status_led_mark_ready(void)
{
}

int papermono_status_led_prepare_sleep(void)
{
	return 0;
}

int papermono_status_led_init(void)
{
	return 0;
}

enum papermono_status_led_state papermono_status_led_get_state(void)
{
	return PAPERMONO_STATUS_LED_CONNECTED;
}

enum papermono_status_led_state papermono_status_led_resolve(
	const struct papermono_status_led_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return PAPERMONO_STATUS_LED_OFF;
	}
	if (snapshot->hard_error) {
		return PAPERMONO_STATUS_LED_ERROR;
	}
	if (snapshot->runtime_fault) {
		return PAPERMONO_STATUS_LED_WARNING;
	}
	if (snapshot->ble_connected) {
		return PAPERMONO_STATUS_LED_CONNECTED;
	}
	return PAPERMONO_STATUS_LED_READY;
}

int papermono_status_led_color(enum papermono_status_led_state state,
	struct papermono_status_led_color *color)
{
	if (color == NULL) {
		return -EINVAL;
	}
	memset(color, 0, sizeof(*color));
	(void)state;
	return 0;
}

const char *papermono_status_led_state_name(enum papermono_status_led_state state)
{
	switch (state) {
	case PAPERMONO_STATUS_LED_OFF:
		return "熄灯";
	case PAPERMONO_STATUS_LED_BOOTING:
		return "启动";
	case PAPERMONO_STATUS_LED_READY:
		return "就绪";
	case PAPERMONO_STATUS_LED_CONNECTING:
		return "连接中";
	case PAPERMONO_STATUS_LED_CONNECTED:
		return "已连接";
	case PAPERMONO_STATUS_LED_BUSY:
		return "忙";
	case PAPERMONO_STATUS_LED_PAIRING:
		return "配对";
	case PAPERMONO_STATUS_LED_WARNING:
		return "注意";
	case PAPERMONO_STATUS_LED_ERROR:
	default:
		return "故障";
	}
}

/* ------------------------------------------------------------------- power */

int papermono_power_get_wakeup_cause(enum papermono_wakeup_cause *cause)
{
	if (cause == NULL) {
		return -EINVAL;
	}
	*cause = PAPERMONO_WAKE_COLD_BOOT;
	return 0;
}

const char *papermono_wakeup_cause_name(enum papermono_wakeup_cause cause)
{
	switch (cause) {
	case PAPERMONO_WAKE_TOUCH:
		return "触摸唤醒";
	case PAPERMONO_WAKE_ESP_TIMER:
		return "定时唤醒";
	case PAPERMONO_WAKE_PMIC_TIMER:
		return "PMIC 定时";
	case PAPERMONO_WAKE_PMIC_BUTTON:
		return "电源键";
	case PAPERMONO_WAKE_PMIC_EXTERNAL:
		return "外部信号";
	case PAPERMONO_WAKE_OTHER:
		return "其他";
	case PAPERMONO_WAKE_COLD_BOOT:
	default:
		return "上电启动";
	}
}

int papermono_power_input_wake_ready(uint32_t timeout_ms)
{
	(void)timeout_ms;
	return 0;
}

int papermono_power_touch_wake_ready(uint32_t timeout_ms)
{
	(void)timeout_ms;
	return 0;
}

int papermono_power_enter_touch_deep_sleep(void)
{
	return -ENOTSUP;
}

int papermono_power_shutdown_button(void)
{
	return -ENOTSUP;
}

int papermono_power_acknowledge_wakeup(enum papermono_wakeup_cause cause)
{
	(void)cause;
	return 0;
}

/* Synthetic weather for the standby fixture: no network on the host. */
int papermono_weather_init(void) { return 0; }
int papermono_weather_request(void) { return 0; }
void papermono_weather_get(struct papermono_weather *state)
{
	if (state == NULL) {
		return;
	}
	memset(state, 0, sizeof(*state));
	state->valid = true;
	state->current_temp = 18;
	state->weather_code = 116;
	state->hourly_count = 8;
	for (uint8_t index = 0U; index < 8U; ++index) {
		state->hourly_temp[index] = (int8_t)(18 + (index % 3));
	}
	state->updated_at = 1789000000;
}

/* Synthetic RSS index and SD body cache. The index mirrors the production
 * struct; bodies are generated on demand, so no host file is read. */
static struct papermono_rss_feed preview_rss;
static int preview_rss_request_result;
static bool preview_rss_body_fail;
static unsigned preview_rss_request_count;

void preview_rss_set_index(const struct papermono_rss_feed *feed)
{
	if (feed == NULL) {
		memset(&preview_rss, 0, sizeof(preview_rss));
	} else {
		preview_rss = *feed;
	}
}

void preview_rss_request_result_set(int result)
{
	preview_rss_request_result = result;
}

void preview_rss_body_fail_set(bool fail) { preview_rss_body_fail = fail; }

unsigned preview_rss_requests(void) { return preview_rss_request_count; }

int papermono_rss_init(void) { return 0; }

int papermono_rss_status(void) { return 0; }

int papermono_rss_request(void)
{
	++preview_rss_request_count;
	return preview_rss_request_result;
}

void papermono_rss_get(struct papermono_rss_feed *state)
{
	if (state != NULL) *state = preview_rss;
}

int papermono_rss_body_load(size_t index, char *buffer, size_t capacity)
{
	static const char sentence[] = "纸页资讯正文用于验证分页行为。";
	static const char ending[] = "——正文结束";
	size_t used = 0U;

	if (preview_rss_body_fail || buffer == NULL || capacity == 0U ||
	    index >= preview_rss.count) {
		return -ENOENT;
	}
	if (index != 0U) {
		(void)snprintf(buffer, capacity, "短文只有一页。");
		return (int)strlen(buffer);
	}
	/* About three 19-line pages, with a recognisable final line. */
	while (used + sizeof(sentence) - 1U < capacity - 1U && used < 2600U) {
		memcpy(buffer + used, sentence, sizeof(sentence) - 1U);
		used += sizeof(sentence) - 1U;
	}
	if (used + sizeof(ending) - 1U < capacity - 1U) {
		memcpy(buffer + used, ending, sizeof(ending) - 1U);
		used += sizeof(ending) - 1U;
	}
	buffer[used] = '\0';
	return (int)used;
}
