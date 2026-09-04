/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <papermono/settings.h>

LOG_MODULE_REGISTER(papermono_settings, LOG_LEVEL_INF);

#if defined(CONFIG_PAPERMONO_UI_IDLE_SLEEP_DEFAULT_MINUTES)
#define PAPERMONO_DEFAULT_SLEEP_MINUTES \
	CONFIG_PAPERMONO_UI_IDLE_SLEEP_DEFAULT_MINUTES
#else
#define PAPERMONO_DEFAULT_SLEEP_MINUTES 10
#endif

#define PAPERMONO_SETTINGS_RECORD_VERSION 1U
#define PAPERMONO_SETTINGS_FLAG_24_HOUR BIT(0)
#define PAPERMONO_SETTINGS_FLAG_WIFI BIT(1)
#define PAPERMONO_SETTINGS_FLAG_READER_RESUME BIT(2)
#define PAPERMONO_SETTINGS_FLAG_MASK (PAPERMONO_SETTINGS_FLAG_24_HOUR | \
	PAPERMONO_SETTINGS_FLAG_WIFI | PAPERMONO_SETTINGS_FLAG_READER_RESUME)

/* All user preferences are committed as one fixed, versioned NVS value. The
 * explicit byte fields avoid persisting compiler-specific enum/bool layouts.
 */
struct papermono_settings_record_v1 {
	uint8_t version;
	uint8_t flags;
	int16_t timezone_offset_minutes;
	uint8_t idle_sleep_minutes;
	uint8_t reader_density;
	uint8_t active_app;
	uint8_t frontlight_percent;
	char timezone[PAPERMONO_TIMEZONE_NAME_MAX];
	char wifi_ssid[PAPERMONO_WIFI_SSID_MAX + 1U];
	char wifi_psk[PAPERMONO_WIFI_PSK_MAX + 1U];
	uint8_t crc_padding[2];
	uint32_t crc32;
};

_Static_assert(sizeof(struct papermono_settings_record_v1) == 144U,
	"settings record layout changed");

static const struct papermono_preferences defaults = {
	.timezone = "Asia/Shanghai",
	.timezone_offset_minutes = 480,
	.idle_sleep_minutes = PAPERMONO_DEFAULT_SLEEP_MINUTES,
	.reader_density = PAPERMONO_READER_DENSITY_STANDARD,
	.active_app = PAPERMONO_APP_HOME,
	.use_24_hour = true,
};
static struct papermono_preferences preferences;
static struct papermono_preferences record_preferences;
static bool record_loaded;
static bool initialized;

static bool sleep_minutes_valid(uint8_t minutes)
{
	return minutes == 0U || minutes == 1U || minutes == 5U ||
		minutes == 10U || minutes == 30U;
}

static int read_exact(size_t len, settings_read_cb read_cb, void *cb_arg,
	void *value, size_t value_size)
{
	int ret;

	if (len != value_size) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, value, value_size);
	return ret < 0 ? ret : (size_t)ret == value_size ? 0 : -EIO;
}

static void settings_record_encode(
	const struct papermono_preferences *source,
	struct papermono_settings_record_v1 *record)
{
	memset(record, 0, sizeof(*record));
	record->version = PAPERMONO_SETTINGS_RECORD_VERSION;
	record->flags = (source->use_24_hour ?
		PAPERMONO_SETTINGS_FLAG_24_HOUR : 0U) |
		(source->wifi_configured ? PAPERMONO_SETTINGS_FLAG_WIFI : 0U) |
		(source->reader_resume ?
			PAPERMONO_SETTINGS_FLAG_READER_RESUME : 0U);
	record->timezone_offset_minutes = source->timezone_offset_minutes;
	record->idle_sleep_minutes = source->idle_sleep_minutes;
	record->frontlight_percent = source->frontlight_percent;
	record->reader_density = (uint8_t)source->reader_density;
	record->active_app = (uint8_t)source->active_app;
	memcpy(record->timezone, source->timezone, sizeof(record->timezone));
	if (source->wifi_configured) {
		memcpy(record->wifi_ssid, source->wifi_ssid,
			sizeof(record->wifi_ssid));
		memcpy(record->wifi_psk, source->wifi_psk,
			sizeof(record->wifi_psk));
	}
	record->crc32 = crc32_ieee((const uint8_t *)record,
		offsetof(struct papermono_settings_record_v1, crc32));
}

static int settings_record_decode(
	const struct papermono_settings_record_v1 *record,
	struct papermono_preferences *decoded)
{
	const size_t timezone_length = strnlen(record->timezone,
		sizeof(record->timezone));
	const size_t ssid_length = strnlen(record->wifi_ssid,
		sizeof(record->wifi_ssid));
	const size_t psk_length = strnlen(record->wifi_psk,
		sizeof(record->wifi_psk));
	const bool wifi_configured =
		(record->flags & PAPERMONO_SETTINGS_FLAG_WIFI) != 0U;

	if (record->crc32 != crc32_ieee((const uint8_t *)record,
		offsetof(struct papermono_settings_record_v1, crc32)) ||
	    record->version != PAPERMONO_SETTINGS_RECORD_VERSION ||
	    record->frontlight_percent > PAPERMONO_FRONTLIGHT_MAX ||
	    record->crc_padding[0] != 0U || record->crc_padding[1] != 0U ||
	    (record->flags & ~PAPERMONO_SETTINGS_FLAG_MASK) != 0U ||
	    timezone_length == 0U ||
	    timezone_length >= sizeof(record->timezone) ||
	    record->timezone_offset_minutes < -720 ||
	    record->timezone_offset_minutes > 840 ||
	    !sleep_minutes_valid(record->idle_sleep_minutes) ||
	    record->reader_density > PAPERMONO_READER_DENSITY_RELAXED ||
	    record->active_app >= PAPERMONO_APP_COUNT ||
	    (wifi_configured &&
	     (ssid_length == 0U || ssid_length > PAPERMONO_WIFI_SSID_MAX ||
	      psk_length < 8U || psk_length > PAPERMONO_WIFI_PSK_MAX))) {
		return -EINVAL;
	}
	*decoded = defaults;
	memcpy(decoded->timezone, record->timezone,
		sizeof(decoded->timezone));
	decoded->timezone_offset_minutes = record->timezone_offset_minutes;
	decoded->idle_sleep_minutes = record->idle_sleep_minutes;
	decoded->frontlight_percent = record->frontlight_percent;
	decoded->reader_density =
		(enum papermono_reader_density)record->reader_density;
	decoded->active_app = (enum papermono_app_id)record->active_app;
	decoded->use_24_hour =
		(record->flags & PAPERMONO_SETTINGS_FLAG_24_HOUR) != 0U;
	decoded->reader_resume =
		(record->flags & PAPERMONO_SETTINGS_FLAG_READER_RESUME) != 0U;
	decoded->wifi_configured = wifi_configured;
	if (wifi_configured) {
		memcpy(decoded->wifi_ssid, record->wifi_ssid,
			sizeof(decoded->wifi_ssid));
		memcpy(decoded->wifi_psk, record->wifi_psk,
			sizeof(decoded->wifi_psk));
	}
	return 0;
}

static int persist_preferences(
	const struct papermono_preferences *candidate, bool force)
{
	struct papermono_settings_record_v1 record;
	int ret;

	if (!force && memcmp(candidate, &preferences, sizeof(*candidate)) == 0) {
		return 0;
	}
	settings_record_encode(candidate, &record);
	ret = settings_save_one("papermono/config", &record, sizeof(record));
	if (ret == 0) {
		preferences = *candidate;
		record_preferences = *candidate;
		record_loaded = true;
	}
	return ret;
}

static int papermono_settings_set(const char *name, size_t len,
	settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "config", &next) && next == NULL) {
		struct papermono_settings_record_v1 record;
		int ret = read_exact(len, read_cb, cb_arg, &record,
			sizeof(record));

		if (ret == 0) {
			ret = settings_record_decode(&record, &record_preferences);
		}
		if (ret < 0) {
			LOG_WRN("Ignoring invalid settings record (%d)", ret);
			return 0;
		}
		record_loaded = true;
		return 0;
	}
	/* Legacy fields remain loadable so existing firmware installations migrate
	 * without losing Wi-Fi or reader state. The commit hook gives a valid v1
	 * record precedence independent of backend enumeration order.
	 */
	if (settings_name_steq(name, "timezone", &next) && next == NULL) {
		char loaded[PAPERMONO_TIMEZONE_NAME_MAX] = { 0 };

		if (len == 0U || len > sizeof(loaded)) {
			return 0;
		}
		const int ret = read_cb(cb_arg, loaded, len);
		if (ret < 0) {
			return ret;
		}
		if ((size_t)ret != len || loaded[len - 1U] != '\0' ||
		    strnlen(loaded, sizeof(loaded)) == 0U) {
			return 0;
		}
		memcpy(preferences.timezone, loaded, sizeof(loaded));
		return 0;
	}
	if (settings_name_steq(name, "tz_offset", &next) && next == NULL) {
		int16_t offset;
		int ret = read_exact(len, read_cb, cb_arg, &offset, sizeof(offset));

		if (ret == 0 && offset >= -720 && offset <= 840) {
			preferences.timezone_offset_minutes = offset;
		}
		return ret;
	}
	if (settings_name_steq(name, "time_24h", &next) && next == NULL) {
		uint8_t value;
		int ret = read_exact(len, read_cb, cb_arg, &value, sizeof(value));

		if (ret == 0 && value <= 1U) {
			preferences.use_24_hour = value != 0U;
		}
		return ret;
	}
	if (settings_name_steq(name, "sleep_min", &next) && next == NULL) {
		uint8_t minutes;
		int ret = read_exact(len, read_cb, cb_arg, &minutes,
			sizeof(minutes));

		if (ret == 0 && sleep_minutes_valid(minutes)) {
			preferences.idle_sleep_minutes = minutes;
		}
		return ret;
	}
	if (settings_name_steq(name, "frontlight", &next) && next == NULL) {
		uint8_t percent;
		int ret = read_exact(len, read_cb, cb_arg, &percent,
			sizeof(percent));

		if (ret == 0 && percent <= PAPERMONO_FRONTLIGHT_MAX) {
			preferences.frontlight_percent = percent;
		}
		return ret;
	}
	if (settings_name_steq(name, "reader_density", &next) && next == NULL) {
		uint8_t density;
		int ret = read_exact(len, read_cb, cb_arg, &density,
			sizeof(density));

		if (ret == 0 && density <= PAPERMONO_READER_DENSITY_RELAXED) {
			preferences.reader_density =
				(enum papermono_reader_density)density;
		}
		return ret;
	}
	if (settings_name_steq(name, "active_app", &next) && next == NULL) {
		uint8_t app;
		int ret = read_exact(len, read_cb, cb_arg, &app, sizeof(app));

		if (ret == 0 && app < PAPERMONO_APP_COUNT) {
			preferences.active_app = (enum papermono_app_id)app;
		}
		return ret;
	}
	if (settings_name_steq(name, "reader_resume", &next) && next == NULL) {
		uint8_t value;
		int ret = read_exact(len, read_cb, cb_arg, &value, sizeof(value));

		if (ret == 0 && value <= 1U) {
			preferences.reader_resume = value != 0U;
		}
		return ret;
	}
	if (settings_name_steq(name, "wifi_ssid", &next) && next == NULL) {
		if (len == 0U || len > sizeof(preferences.wifi_ssid)) {
			return 0;
		}
		char loaded[PAPERMONO_WIFI_SSID_MAX + 1U] = { 0 };
		const int ret = read_cb(cb_arg, loaded, len);
		if (ret < 0) {
			return ret;
		}
		if ((size_t)ret != len || loaded[len - 1U] != '\0') {
			return 0;
		}
		memcpy(preferences.wifi_ssid, loaded, sizeof(loaded));
		return 0;
	}
	if (settings_name_steq(name, "wifi_psk", &next) && next == NULL) {
		if (len == 0U || len > sizeof(preferences.wifi_psk)) {
			return 0;
		}
		char loaded[PAPERMONO_WIFI_PSK_MAX + 1U] = { 0 };
		const int ret = read_cb(cb_arg, loaded, len);
		if (ret < 0) {
			return ret;
		}
		if ((size_t)ret != len || loaded[len - 1U] != '\0') {
			return 0;
		}
		memcpy(preferences.wifi_psk, loaded, sizeof(loaded));
		return 0;
	}
	return -ENOENT;
}

static int papermono_settings_commit(void)
{
	if (record_loaded) {
		preferences = record_preferences;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(papermono, "papermono", NULL,
	papermono_settings_set, papermono_settings_commit, NULL);

int papermono_settings_init(void)
{
	int ret;

	if (initialized) {
		return 0;
	}
	preferences = defaults;
	record_preferences = defaults;
	record_loaded = false;
	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("Settings subsystem init failed (%d)", ret);
		return ret;
	}
	ret = settings_load_subtree("papermono");
	if (ret < 0) {
		LOG_ERR("PaperMono settings load failed (%d)", ret);
		return ret;
	}
	const size_t ssid_length = strnlen(preferences.wifi_ssid,
		sizeof(preferences.wifi_ssid));
	const size_t psk_length = strnlen(preferences.wifi_psk,
		sizeof(preferences.wifi_psk));
	preferences.wifi_configured = preferences.wifi_configured ||
		(ssid_length > 0U && ssid_length <= PAPERMONO_WIFI_SSID_MAX &&
		 psk_length >= 8U && psk_length <= PAPERMONO_WIFI_PSK_MAX);
	if (!preferences.wifi_configured) {
		memset(preferences.wifi_ssid, 0, sizeof(preferences.wifi_ssid));
		memset(preferences.wifi_psk, 0, sizeof(preferences.wifi_psk));
	}
	if (!record_loaded) {
		ret = persist_preferences(&preferences, true);
		if (ret < 0) {
			LOG_WRN("Unable to migrate legacy settings (%d)", ret);
		}
	}
	initialized = true;
	LOG_INF("Settings ready: tz=%s sleep=%u min app=%u schema=%u",
		preferences.timezone, preferences.idle_sleep_minutes,
		preferences.active_app, PAPERMONO_SETTINGS_RECORD_VERSION);
	return 0;
}

const struct papermono_preferences *papermono_settings_get(void)
{
	return &preferences;
}

int papermono_settings_set_active_app(enum papermono_app_id app)
{
	if (app >= PAPERMONO_APP_COUNT) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	candidate.active_app = app;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_idle_sleep(uint8_t minutes)
{
	if (!sleep_minutes_valid(minutes)) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	candidate.idle_sleep_minutes = minutes;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_frontlight(uint8_t percent)
{
	if (percent > PAPERMONO_FRONTLIGHT_MAX) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	candidate.frontlight_percent = percent;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_time_format(bool use_24_hour)
{
	struct papermono_preferences candidate = preferences;
	candidate.use_24_hour = use_24_hour;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_reader_density(
	enum papermono_reader_density density)
{
	if (density > PAPERMONO_READER_DENSITY_RELAXED) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	candidate.reader_density = density;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_reader_resume(bool resume)
{
	struct papermono_preferences candidate = preferences;
	candidate.reader_resume = resume;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_timezone(const char *name, int16_t offset_minutes)
{
	if (name == NULL) {
		return -EINVAL;
	}
	const size_t length = strnlen(name, sizeof(preferences.timezone));
	if (length == 0U || length >= sizeof(preferences.timezone) ||
	    offset_minutes < -720 || offset_minutes > 840) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	memset(candidate.timezone, 0, sizeof(candidate.timezone));
	memcpy(candidate.timezone, name, length);
	candidate.timezone_offset_minutes = offset_minutes;
	return persist_preferences(&candidate, false);
}

int papermono_settings_set_wifi(const char *ssid, const char *psk)
{
	const size_t ssid_length = ssid == NULL ? 0U :
		strnlen(ssid, sizeof(preferences.wifi_ssid));
	const size_t psk_length = psk == NULL ? 0U :
		strnlen(psk, sizeof(preferences.wifi_psk));

	if (ssid_length == 0U || ssid_length > PAPERMONO_WIFI_SSID_MAX ||
	    psk_length < 8U || psk_length > PAPERMONO_WIFI_PSK_MAX) {
		return -EINVAL;
	}
	struct papermono_preferences candidate = preferences;
	memset(candidate.wifi_ssid, 0, sizeof(candidate.wifi_ssid));
	memset(candidate.wifi_psk, 0, sizeof(candidate.wifi_psk));
	memcpy(candidate.wifi_ssid, ssid, ssid_length);
	memcpy(candidate.wifi_psk, psk, psk_length);
	candidate.wifi_configured = true;
	return persist_preferences(&candidate, false);
}

int papermono_settings_clear_wifi(void)
{
	struct papermono_preferences candidate = preferences;
	memset(candidate.wifi_ssid, 0, sizeof(candidate.wifi_ssid));
	memset(candidate.wifi_psk, 0, sizeof(candidate.wifi_psk));
	candidate.wifi_configured = false;
	return persist_preferences(&candidate, false);
}

int papermono_settings_reset(void)
{
	static const char *const legacy_keys[] = {
		"papermono/timezone",
		"papermono/tz_offset",
		"papermono/time_24h",
		"papermono/sleep_min",
		"papermono/frontlight",
		"papermono/reader_density",
		"papermono/reader_resume",
		"papermono/active_app",
		"papermono/wifi_ssid",
		"papermono/wifi_psk",
	};
	int ret = persist_preferences(&defaults, true);

	if (ret < 0) {
		return ret;
	}
	for (size_t index = 0U; index < ARRAY_SIZE(legacy_keys); ++index) {
		ret = settings_delete(legacy_keys[index]);
		if (ret < 0 && ret != -ENOENT) {
			LOG_WRN("Unable to remove legacy setting %s (%d)",
				legacy_keys[index], ret);
		}
	}
	return 0;
}
