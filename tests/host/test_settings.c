/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>

#include <papermono/settings.h>

int settings_host_set_papermono(const char *key, size_t len,
	settings_read_cb read_cb, void *cb_arg);
int settings_host_commit_papermono(void);

struct fake_setting {
	const char *key;
	const void *value;
	size_t length;
};

static const char empty_timezone[] = "";
static const int16_t invalid_offset = 1200;
static const uint8_t invalid_bool = 7U;
static const uint8_t invalid_sleep = 7U;
static const uint8_t invalid_density = 9U;
static const uint8_t invalid_app = 255U;
static const uint8_t invalid_reader_resume = 2U;
static const char wifi_ssid[] = "PaperMono-Test";
static const char wifi_psk[] = "12345678";
static const uint8_t invalid_config[144];
static unsigned int save_count;
static int save_result;
static char saved_key[64];
static uint8_t saved_value[256];
static size_t saved_length;

static const struct fake_setting loaded_settings[] = {
	{ "timezone", empty_timezone, sizeof(empty_timezone) },
	{ "tz_offset", &invalid_offset, sizeof(invalid_offset) },
	{ "time_24h", &invalid_bool, sizeof(invalid_bool) },
	{ "sleep_min", &invalid_sleep, sizeof(invalid_sleep) },
	{ "reader_density", &invalid_density, sizeof(invalid_density) },
	{ "active_app", &invalid_app, sizeof(invalid_app) },
	{ "reader_resume", &invalid_reader_resume,
		sizeof(invalid_reader_resume) },
	{ "wifi_ssid", wifi_ssid, sizeof(wifi_ssid) },
	{ "wifi_psk", wifi_psk, sizeof(wifi_psk) },
	{ "config", invalid_config, sizeof(invalid_config) },
};

static int read_fake(void *cb_arg, void *data, size_t len)
{
	const struct fake_setting *setting = cb_arg;

	if (len != setting->length) {
		return -1;
	}
	memcpy(data, setting->value, len);
	return (int)len;
}

int settings_subsys_init(void)
{
	return 0;
}

int settings_load_subtree(const char *subtree)
{
	assert(strcmp(subtree, "papermono") == 0);
	for (size_t index = 0U;
	     index < sizeof(loaded_settings) / sizeof(loaded_settings[0]); ++index) {
		const struct fake_setting *setting = &loaded_settings[index];
		int ret = settings_host_set_papermono(setting->key,
			setting->length, read_fake, (void *)setting);

		if (ret < 0) {
			return ret;
		}
	}
	return settings_host_commit_papermono();
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	assert(key != NULL);
	assert(value != NULL || length == 0U);
	++save_count;
	if (save_result < 0) {
		return save_result;
	}
	assert(strlen(key) < sizeof(saved_key));
	assert(length <= sizeof(saved_value));
	strcpy(saved_key, key);
	memcpy(saved_value, value, length);
	saved_length = length;
	return 0;
}

int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param)
{
	(void)subtree;
	(void)callback;
	(void)param;
	return 0;
}

int settings_delete(const char *key)
{
	assert(key != NULL);
	return 0;
}

int main(void)
{
	assert(papermono_settings_init() == 0);
	const struct papermono_preferences *preferences = papermono_settings_get();

	/* Corrupt persisted fields are ignored independently instead of replacing
	 * safe defaults or aborting the complete settings load.
	 */
	assert(strcmp(preferences->timezone, "Asia/Shanghai") == 0);
	assert(preferences->timezone_offset_minutes == 480);
	assert(preferences->use_24_hour);
	assert(preferences->idle_sleep_minutes == 10U);
	assert(preferences->reader_density == PAPERMONO_READER_DENSITY_STANDARD);
	assert(preferences->active_app == PAPERMONO_APP_HOME);
	assert(!preferences->reader_resume);
	assert(preferences->wifi_configured);
	assert(strcmp(preferences->wifi_ssid, wifi_ssid) == 0);
	assert(strcmp(preferences->wifi_psk, wifi_psk) == 0);
	/* The invalid new-format record is ignored, then legacy keys are migrated
	 * into one atomic versioned value.
	 */
	assert(save_count == 1U);
	assert(strcmp(saved_key, "papermono/config") == 0);
	assert(saved_length == sizeof(invalid_config));
	struct fake_setting saved_config = {
		.key = "config",
		.value = saved_value,
		.length = saved_length,
	};
	static const uint8_t legacy_sleep_override = 1U;
	struct fake_setting legacy_sleep = {
		.key = "sleep_min",
		.value = &legacy_sleep_override,
		.length = sizeof(legacy_sleep_override),
	};
	assert(settings_host_set_papermono(legacy_sleep.key,
		legacy_sleep.length, read_fake, &legacy_sleep) == 0);
	assert(settings_host_set_papermono(saved_config.key,
		saved_config.length, read_fake, &saved_config) == 0);
	assert(settings_host_commit_papermono() == 0);
	assert(papermono_settings_get()->idle_sleep_minutes == 10U);

	assert(papermono_settings_set_idle_sleep(0U) == 0);
	assert(papermono_settings_set_idle_sleep(1U) == 0);
	assert(papermono_settings_set_idle_sleep(5U) == 0);
	assert(papermono_settings_set_idle_sleep(10U) == 0);
	assert(papermono_settings_set_idle_sleep(30U) == 0);
	assert(papermono_settings_set_idle_sleep(2U) < 0);
	assert(papermono_settings_set_frontlight(0U) == 0);
	assert(papermono_settings_set_frontlight(25U) == 0);
	assert(papermono_settings_set_frontlight(50U) == 0);
	assert(papermono_settings_set_frontlight(75U) == 0);
	assert(papermono_settings_set_frontlight(100U) == 0);
	assert(papermono_settings_set_frontlight(101U) == -EINVAL);
	save_result = -EIO;
	assert(papermono_settings_set_frontlight(25U) == -EIO);
	assert(papermono_settings_get()->frontlight_percent == 100U);
	save_result = 0;
	save_result = -EIO;
	assert(papermono_settings_set_idle_sleep(5U) == -EIO);
	assert(papermono_settings_get()->idle_sleep_minutes == 30U);
	save_result = 0;
	assert(papermono_settings_set_timezone("UTC", 0) == 0);
	assert(papermono_settings_set_timezone("bad", 841) < 0);
	save_result = -EIO;
	assert(papermono_settings_set_timezone("Asia/Tokyo", 540) == -EIO);
	assert(strcmp(papermono_settings_get()->timezone, "UTC") == 0);
	assert(papermono_settings_get()->timezone_offset_minutes == 0);
	save_result = 0;
	assert(papermono_settings_set_wifi("ssid", "short") < 0);
	assert(papermono_settings_set_wifi("ssid", "valid-pass") == 0);
	save_result = -EIO;
	assert(papermono_settings_clear_wifi() == -EIO);
	assert(papermono_settings_get()->wifi_configured);
	assert(strcmp(papermono_settings_get()->wifi_ssid, "ssid") == 0);
	assert(strcmp(papermono_settings_get()->wifi_psk, "valid-pass") == 0);
	save_result = 0;
	assert(papermono_settings_clear_wifi() == 0);
	assert(!papermono_settings_get()->wifi_configured);
	assert(papermono_settings_set_reader_resume(true) == 0);
	assert(papermono_settings_get()->reader_resume);
	const unsigned int resume_true_saves = save_count;
	assert(papermono_settings_set_reader_resume(true) == 0);
	assert(save_count == resume_true_saves);
	assert(papermono_settings_set_reader_resume(false) == 0);
	assert(!papermono_settings_get()->reader_resume);
	const unsigned int resume_false_saves = save_count;
	assert(papermono_settings_set_reader_resume(false) == 0);
	assert(save_count == resume_false_saves);
	assert(papermono_settings_set_idle_sleep(1U) == 0);
	struct papermono_preferences before_reset =
		*papermono_settings_get();
	save_result = -EIO;
	assert(papermono_settings_reset() == -EIO);
	assert(memcmp(papermono_settings_get(), &before_reset,
		sizeof(before_reset)) == 0);
	save_result = 0;
	assert(papermono_settings_reset() == 0);
	assert(papermono_settings_get()->idle_sleep_minutes == 10U);
	assert(papermono_settings_get()->frontlight_percent == 0U);
	assert(!papermono_settings_get()->wifi_configured);

	puts("settings atomic persistence: PASS");
	return 0;
}
