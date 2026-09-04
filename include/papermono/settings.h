/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_SETTINGS_H_
#define PAPERMONO_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#include <papermono/app.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_TIMEZONE_NAME_MAX 32U
#define PAPERMONO_WIFI_SSID_MAX 32U
#define PAPERMONO_WIFI_PSK_MAX 64U

enum papermono_reader_density {
	PAPERMONO_READER_DENSITY_COMPACT,
	PAPERMONO_READER_DENSITY_STANDARD,
	PAPERMONO_READER_DENSITY_RELAXED,
};

struct papermono_preferences {
	char timezone[PAPERMONO_TIMEZONE_NAME_MAX];
	char wifi_ssid[PAPERMONO_WIFI_SSID_MAX + 1U];
	char wifi_psk[PAPERMONO_WIFI_PSK_MAX + 1U];
	int16_t timezone_offset_minutes;
	uint8_t idle_sleep_minutes;
	/* Frontlight duty cycle in percent. The panel is unreadable in a dark
	 * room at 0, and the driver applies a perceptual curve on top.
	 */
	uint8_t frontlight_percent;
	enum papermono_reader_density reader_density;
	enum papermono_app_id active_app;
	bool use_24_hour;
	bool wifi_configured;
	bool reader_resume;
};

#define PAPERMONO_FRONTLIGHT_STEP 25U
#define PAPERMONO_FRONTLIGHT_MAX  100U

int papermono_settings_init(void);
const struct papermono_preferences *papermono_settings_get(void);
int papermono_settings_set_active_app(enum papermono_app_id app);
int papermono_settings_set_idle_sleep(uint8_t minutes);
int papermono_settings_set_frontlight(uint8_t percent);
int papermono_settings_set_time_format(bool use_24_hour);
int papermono_settings_set_reader_density(
	enum papermono_reader_density density);
int papermono_settings_set_reader_resume(bool resume);
int papermono_settings_set_timezone(const char *name, int16_t offset_minutes);
int papermono_settings_set_wifi(const char *ssid, const char *psk);
int papermono_settings_clear_wifi(void);
int papermono_settings_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_SETTINGS_H_ */
