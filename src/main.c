/* SPDX-License-Identifier: GPL-3.0-only */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT) && \
	CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT > 1
#define PAPERMONO_POWER_HIL_REPEAT_ENABLED 1
#else
#define PAPERMONO_POWER_HIL_REPEAT_ENABLED 0
#endif

#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_IMU) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_POWER_BUTTON)
#define PAPERMONO_POWER_HIL_ENABLED 1
#else
#define PAPERMONO_POWER_HIL_ENABLED 0
#endif

#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	PAPERMONO_POWER_HIL_REPEAT_ENABLED
#include <papermono/m5pm1.h>
#endif
#if defined(CONFIG_PAPERMONO_DRIVER_SELFTEST)
#include <papermono/driver_selftest.h>
#endif
#if defined(CONFIG_PAPERMONO_POWER_CONTROL)
#include <papermono/power.h>
#endif
#include <papermono/ui.h>
#include <papermono/status_led.h>
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
#include <papermono/content_server.h>
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
#include <papermono/hid.h>
#include <papermono/sync.h>
#endif
#if defined(CONFIG_PAPERMONO_BLE_MIC)
#include <papermono/ble_mic.h>
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#if PAPERMONO_POWER_HIL_ENABLED
static bool power_hil_wake_matches(enum papermono_wakeup_cause cause)
{
#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER)
	return cause == PAPERMONO_WAKE_ESP_TIMER;
#elif defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH)
	return cause == PAPERMONO_WAKE_TOUCH;
#elif defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_POWER_BUTTON)
	return cause == PAPERMONO_WAKE_PMIC_IRQ;
#elif defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON)
	return cause == PAPERMONO_WAKE_PMIC_BUTTON;
#elif defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
	return cause == PAPERMONO_WAKE_PMIC_TIMER;
#else
	return cause == PAPERMONO_WAKE_PMIC_EXTERNAL;
#endif
}
#endif

#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	PAPERMONO_POWER_HIL_REPEAT_ENABLED
static const struct device *const power_hil_pmic =
	DEVICE_DT_GET(DT_NODELABEL(m5pm1));
#endif

#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
#define POWER_HIL_REPEAT_MARKER_OFFSET 20U
#define POWER_HIL_REPEAT_MAGIC_0 0x50U
#define POWER_HIL_REPEAT_MAGIC_1 0x52U

static int power_hil_repeat_get(uint8_t *cycle)
{
	uint8_t marker[4];
	int ret = m5pm1_rtc_ram_read(power_hil_pmic,
		POWER_HIL_REPEAT_MARKER_OFFSET, marker, sizeof(marker));

	if (ret < 0) {
		return ret;
	}
	if (marker[0] != POWER_HIL_REPEAT_MAGIC_0 ||
	    marker[1] != POWER_HIL_REPEAT_MAGIC_1 ||
	    marker[3] != (uint8_t)(0xA5U ^ marker[2]) ||
	    marker[2] >= CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT) {
		return -ENOENT;
	}
	*cycle = marker[2];
	return 0;
}

static int power_hil_repeat_set(uint8_t cycle)
{
	const uint8_t marker[] = {
		POWER_HIL_REPEAT_MAGIC_0,
		POWER_HIL_REPEAT_MAGIC_1,
		cycle,
		(uint8_t)(0xA5U ^ cycle),
	};

	return m5pm1_rtc_ram_write(power_hil_pmic,
		POWER_HIL_REPEAT_MARKER_OFFSET, marker, sizeof(marker), true);
}

static int power_hil_repeat_clear(void)
{
	static const uint8_t clear[4];

	return m5pm1_rtc_ram_write(power_hil_pmic,
		POWER_HIL_REPEAT_MARKER_OFFSET, clear, sizeof(clear), true);
}
#endif

#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
#define POWER_HIL_RTC_MARKER_OFFSET 24U

enum power_hil_rtc_marker_state {
	POWER_HIL_RTC_MARKER_NONE,
	POWER_HIL_RTC_MARKER_ARMED,
	POWER_HIL_RTC_MARKER_PASS,
	POWER_HIL_RTC_MARKER_FAIL,
};

static const uint8_t power_hil_rtc_marker_armed[] = {
	0x50U, 0x48U, 0x49U, 0x4cU,
};
static const uint8_t power_hil_rtc_marker_pass[] = {
	0x50U, 0x41U, 0x53U, 0x53U,
};
static const uint8_t power_hil_rtc_marker_fail[] = {
	0x46U, 0x41U, 0x49U, 0x4cU,
};

static int power_hil_rtc_marker_set(enum power_hil_rtc_marker_state state)
{
	static const uint8_t clear[sizeof(power_hil_rtc_marker_armed)];
	const uint8_t *value = clear;

	switch (state) {
	case POWER_HIL_RTC_MARKER_ARMED:
		value = power_hil_rtc_marker_armed;
		break;
	case POWER_HIL_RTC_MARKER_PASS:
		value = power_hil_rtc_marker_pass;
		break;
	case POWER_HIL_RTC_MARKER_FAIL:
		value = power_hil_rtc_marker_fail;
		break;
	case POWER_HIL_RTC_MARKER_NONE:
		break;
	}

	return m5pm1_rtc_ram_write(power_hil_pmic,
		POWER_HIL_RTC_MARKER_OFFSET, value,
		sizeof(power_hil_rtc_marker_armed),
		true);
}

static int power_hil_rtc_marker_get(enum power_hil_rtc_marker_state *state)
{
	uint8_t value[sizeof(power_hil_rtc_marker_armed)];
	int ret;

	*state = POWER_HIL_RTC_MARKER_NONE;
	ret = m5pm1_rtc_ram_read(power_hil_pmic,
		POWER_HIL_RTC_MARKER_OFFSET, value, sizeof(value));
	if (ret < 0) {
		return ret;
	}
	if (memcmp(value, power_hil_rtc_marker_armed, sizeof(value)) == 0) {
		*state = POWER_HIL_RTC_MARKER_ARMED;
	} else if (memcmp(value, power_hil_rtc_marker_pass,
			  sizeof(value)) == 0) {
		*state = POWER_HIL_RTC_MARKER_PASS;
	} else if (memcmp(value, power_hil_rtc_marker_fail,
			  sizeof(value)) == 0) {
		*state = POWER_HIL_RTC_MARKER_FAIL;
	}
	return 0;
}
#endif

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int ret;
#if CONFIG_PAPERMONO_POWER_HIL_USB_RECONNECT_DELAY_MS > 0
	/* Power HIL is observed through the board's only debug transport. Give
	 * USB Serial/JTAG time to enumerate before emitting application logs.
	 */
	k_msleep(CONFIG_PAPERMONO_POWER_HIL_USB_RECONNECT_DELAY_MS);
#endif
	#if PAPERMONO_POWER_HIL_ENABLED
	bool display_refresh_ok = false;
	#endif

#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	ret = papermono_status_led_init();
	if (ret < 0) {
		LOG_WRN("Device status LED unavailable (%d)", ret);
	}
#endif

#if defined(CONFIG_PAPERMONO_POWER_CONTROL)
	enum papermono_wakeup_cause wake_cause = PAPERMONO_WAKE_COLD_BOOT;
#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
	bool power_hil_rtc_armed = false;
	enum power_hil_rtc_marker_state power_hil_rtc_state =
		POWER_HIL_RTC_MARKER_NONE;
#endif

	ret = papermono_power_get_wakeup_cause(&wake_cause);
#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
	if (ret == 0) {
		ret = power_hil_rtc_marker_get(&power_hil_rtc_state);
	}
	if (ret == 0 &&
	    (power_hil_rtc_state == POWER_HIL_RTC_MARKER_PASS ||
	     power_hil_rtc_state == POWER_HIL_RTC_MARKER_FAIL)) {
		if (power_hil_rtc_state == POWER_HIL_RTC_MARKER_PASS) {
			LOG_INF("Previous RTC power HIL result: PASS");
		} else {
			LOG_ERR("Previous RTC power HIL result: FAIL");
		}
		(void)power_hil_rtc_marker_set(POWER_HIL_RTC_MARKER_NONE);
		k_sleep(K_FOREVER);
		return 0;
	}
	if (ret == 0 &&
	    power_hil_rtc_state == POWER_HIL_RTC_MARKER_ARMED) {
		power_hil_rtc_armed = true;
		ret = power_hil_rtc_marker_set(POWER_HIL_RTC_MARKER_NONE);
	}
#endif
	if (ret == 0 && wake_cause != PAPERMONO_WAKE_COLD_BOOT) {
		LOG_INF("PaperMono resumed from wake source %d (%s)", wake_cause,
			papermono_wakeup_cause_name(wake_cause));
		ret = papermono_power_acknowledge_wakeup(wake_cause);
		if (ret < 0) {
			LOG_WRN("Failed to acknowledge wake source %d (%d)",
				wake_cause, ret);
		}
	}
#endif

	if (!device_is_ready(display)) {
#if PAPERMONO_POWER_HIL_ENABLED
		LOG_WRN("PaperMono display device is not ready; continuing power HIL");
		ret = -ENODEV;
#else
		LOG_ERR("PaperMono display device is not ready");
	#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
		papermono_status_led_report_startup_fault(true);
	#endif
		return -ENODEV;
#endif
	} else {
		ret = papermono_ui_init(display);
	}
#if PAPERMONO_POWER_HIL_ENABLED
	display_refresh_ok = ret == 0;
#endif
	if (ret < 0) {
#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
		papermono_status_led_report_startup_fault(true);
#endif
#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_IMU) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_POWER_BUTTON)
		LOG_WRN("Display refresh failed (%d); continuing power HIL", ret);
#else
		LOG_ERR("Display refresh failed (%d)", ret);
		return ret;
#endif
	} else {
		LOG_INF("PaperMono LVGL page rendered");
	}

#if defined(CONFIG_PAPERMONO_DRIVER_SELFTEST)
	if (ret < 0) {
		LOG_WRN("Skipping driver self-test after display failure");
	}
#if PAPERMONO_POWER_HIL_ENABLED
	else if (power_hil_wake_matches(wake_cause)) {
		/* A matched wake is accepted on wake-source identity plus a fresh
		 * display refresh. Re-running every unrelated peripheral check here
		 * can hide the actual wake result (for example, a non-responsive
		 * optional charger register interface) and delays USB-only capture.
		 */
		LOG_INF("Power HIL wake path: display restored; skipping unrelated driver self-test");
	}
#endif
	else {
		ret = papermono_driver_selftest_run(display);
		if (ret < 0) {
#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_IMU) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_POWER_BUTTON)
			LOG_WRN("Driver self-test failed (%d); continuing isolated power HIL",
				ret);
#else
			return ret;
#endif
		}
	}
#endif

#if PAPERMONO_POWER_HIL_ENABLED
	bool power_hil_wake_observed = power_hil_wake_matches(wake_cause);
	bool power_hil_wake_verified = power_hil_wake_observed &&
		display_refresh_ok;
	bool power_hil_repeat_pending = false;
#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
	uint8_t power_hil_repeat_cycle = 0U;
	int repeat_ret = wake_cause == PAPERMONO_WAKE_COLD_BOOT ? -ENOENT :
		power_hil_repeat_get(&power_hil_repeat_cycle);

	if (repeat_ret < 0 && repeat_ret != -ENOENT) {
		LOG_ERR("Unable to read power HIL repeat marker (%d)", repeat_ret);
		power_hil_wake_verified = false;
	}
#endif

#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
	power_hil_wake_observed = power_hil_wake_observed &&
		power_hil_rtc_armed;
	power_hil_wake_verified = power_hil_wake_observed &&
		display_refresh_ok;
#endif
	if (power_hil_wake_observed) {
#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
		ret = power_hil_rtc_marker_set(power_hil_wake_verified ?
			POWER_HIL_RTC_MARKER_PASS : POWER_HIL_RTC_MARKER_FAIL);
		if (ret < 0) {
			LOG_WRN("Failed to persist RTC power HIL result (%d)", ret);
		}
#endif
		#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
		if (power_hil_wake_verified) {
			++power_hil_repeat_cycle;
			if (power_hil_repeat_cycle <
			    CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT) {
				ret = power_hil_repeat_set(power_hil_repeat_cycle);
				power_hil_repeat_pending = ret == 0;
				if (ret < 0) {
					power_hil_wake_verified = false;
					LOG_ERR("Unable to persist power HIL cycle (%d)",
						ret);
				}
			} else {
				(void)power_hil_repeat_clear();
			}
		}
		#endif
	#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
		if (power_hil_wake_verified && power_hil_repeat_pending) {
			LOG_INF("Power HIL wake/display cycle %u/%u verified; rearming",
				power_hil_repeat_cycle,
				CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT);
		} else
	#endif
		if (power_hil_wake_verified) {
		#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
			LOG_INF("Power HIL endurance PASS: %u/%u wake/display cycles verified",
				power_hil_repeat_cycle,
				CONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT);
		#else
			LOG_INF("Power HIL wake verified; the power action will not repeat");
		#endif
		} else {
			LOG_ERR("Power HIL wake observed, but display recovery failed; "
				"the power action will not repeat");
		}
	}
	if (!power_hil_wake_observed || power_hil_repeat_pending) {
		if (wake_cause != PAPERMONO_WAKE_COLD_BOOT &&
		    !power_hil_repeat_pending) {
#if defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
			if (power_hil_wake_matches(wake_cause) &&
			    !power_hil_rtc_armed) {
				LOG_WRN("Wake source %d has no active RTC HIL marker; "
					"running a fresh cycle", wake_cause);
			} else
#endif
			{
			LOG_WRN("Wake source %d does not match this power HIL profile; "
				"running the configured action", wake_cause);
			}
		}
		LOG_WRN("Power HIL action starts in %u seconds",
			CONFIG_PAPERMONO_POWER_HIL_DELAY_SECONDS);
	#if PAPERMONO_POWER_HIL_REPEAT_ENABLED
		if (!power_hil_repeat_pending) {
			power_hil_repeat_cycle = 0U;
			ret = power_hil_repeat_set(power_hil_repeat_cycle);
			if (ret < 0) {
				LOG_ERR("Unable to arm power HIL repeat marker (%d)", ret);
				return ret;
			}
		}
	#endif
		k_sleep(K_SECONDS(CONFIG_PAPERMONO_POWER_HIL_DELAY_SECONDS));
#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER)
		ret = papermono_power_enter_timed_deep_sleep(
			CONFIG_PAPERMONO_POWER_HIL_WAKE_SECONDS);
#elif defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_POWER_BUTTON)
		ret = papermono_power_enter_touch_deep_sleep();
#elif defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON)
		ret = papermono_power_shutdown_button();
#elif defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC)
		uint32_t actual_ms;

		ret = power_hil_rtc_marker_set(POWER_HIL_RTC_MARKER_ARMED);
		if (ret == 0) {
			ret = papermono_power_shutdown_rtc(
				CONFIG_PAPERMONO_POWER_HIL_WAKE_SECONDS * 1000U,
				&actual_ms);
		}
		if (ret < 0) {
			(void)power_hil_rtc_marker_set(
				POWER_HIL_RTC_MARKER_NONE);
		}
#else
		ret = papermono_power_shutdown_imu();
#endif
		if (ret < 0) {
			LOG_ERR("Power HIL action failed (%d)", ret);
			return ret;
		}
	}
#endif

#if defined(CONFIG_PAPERMONO_PARTIAL_DEMO)
	for (uint32_t update = 1U;
	     update <= CONFIG_PAPERMONO_PARTIAL_DEMO_UPDATES; ++update) {
		k_msleep(CONFIG_PAPERMONO_PARTIAL_DEMO_INTERVAL_MS);
		ret = papermono_ui_demo_update(
			update, CONFIG_PAPERMONO_PARTIAL_DEMO_UPDATES);
		if (ret < 0) {
			LOG_ERR("Dashboard update %u failed (%d)", update, ret);
			return ret;
		}
	}
	LOG_INF("Partial-refresh HIL demo complete");
#endif

#if defined(CONFIG_PAPERMONO_INTERACTIVE_UI)
	/*
	 * Establish Wi-Fi first.  ESP32-S3 coexistence initialization can restart
	 * the shared radio while Bluetooth comes up; waiting for an externally
	 * useful Wi-Fi state gives the SoftAP/STA path a deterministic baseline
	 * before BLE advertising is enabled.
	 */
	#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	ret = papermono_content_server_init();
	if (ret < 0) {
		LOG_WRN("Local content center unavailable (%d)", ret);
		papermono_status_led_report_startup_fault(true);
	} else {
		struct papermono_content_server_state state;

		for (uint8_t attempt = 0U; attempt < 40U; ++attempt) {
			papermono_content_server_get_state(&state);
			if (state.ap_verified || state.station_connected) {
				LOG_INF("Wi-Fi radio baseline ready before BLE startup");
				break;
			}
			k_msleep(100);
		}
	}
	#endif
	#if defined(CONFIG_PAPERMONO_BLE_HID)
	ret = papermono_hid_init();
	if (ret < 0) {
		LOG_WRN("BLE HID remote unavailable (%d)", ret);
		papermono_status_led_report_startup_fault(true);
	}
	#endif
	#if defined(CONFIG_PAPERMONO_BLE_SYNC)
	ret = papermono_sync_init();
	if (ret < 0) {
		LOG_WRN("BLE small-data sync unavailable (%d)", ret);
		papermono_status_led_report_startup_fault(true);
	}
	#endif
	#if defined(CONFIG_PAPERMONO_BLE_MIC)
	ret = papermono_ble_mic_init();
	if (ret < 0) {
		LOG_WRN("BLE voice microphone unavailable (%d)", ret);
		papermono_status_led_report_startup_fault(true);
	}
	#endif
	papermono_status_led_mark_ready();
	ret = papermono_ui_run();
	papermono_status_led_report_startup_fault(true);
	LOG_ERR("Interactive UI stopped (%d)", ret);
	return ret;
#endif

	k_sleep(K_FOREVER);
	return 0;
}
