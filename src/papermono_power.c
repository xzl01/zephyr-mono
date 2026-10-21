/*
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include <esp_sleep.h>

#include <papermono/m5ioe1.h>
#include <papermono/m5pm1.h>
#include <papermono/power.h>
#include <papermono/ssd1677.h>
#include <papermono/status_led.h>
#if defined(CONFIG_PAPERMONO_PDM_MIC)
#include <papermono/pdm_mic.h>
#endif
#if defined(CONFIG_PAPERMONO_RX8130_TIMER)
#include <papermono/rx8130.h>
#endif
#if defined(CONFIG_PAPERMONO_ST25R3916)
#include <papermono/st25r3916.h>
#endif

LOG_MODULE_REGISTER(papermono_power, LOG_LEVEL_INF);

static const struct device *const pmic = DEVICE_DT_GET(DT_NODELABEL(m5pm1));
static const struct device *const ioe = DEVICE_DT_GET(DT_NODELABEL(m5ioe1));
static const struct device *const display =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
static const struct gpio_dt_spec touch_irq =
	GPIO_DT_SPEC_GET(DT_NODELABEL(papermono_touch), int_gpios);
static const struct gpio_dt_spec button_left = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button_right = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec pmic_irq =
	GPIO_DT_SPEC_GET(DT_NODELABEL(m5pm1), interrupt_gpios);

#define BUTTON_WAKE_MASK (BIT64(DT_GPIO_PIN(DT_ALIAS(sw0), gpios)) | \
	BIT64(DT_GPIO_PIN(DT_ALIAS(sw1), gpios)))
#define PMIC_WAKE_MASK BIT64(DT_GPIO_PIN(DT_NODELABEL(m5pm1), interrupt_gpios))

/* ESP EXT1 masks use physical RTC GPIO numbers, not expander pin numbers.
 * GPIO_INT_WAKEUP also asks Zephyr to configure the independent RTC pull-up.
 */
#define ASSERT_RTC_WAKE_INPUT(node, prop) \
	BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(node, prop), DT_NODELABEL(gpio0)) && \
		DT_GPIO_PIN(node, prop) <= 21 && \
		(DT_GPIO_FLAGS(node, prop) & \
		 (GPIO_ACTIVE_LOW | GPIO_PULL_UP | GPIO_INT_WAKEUP)) == \
		(GPIO_ACTIVE_LOW | GPIO_PULL_UP | GPIO_INT_WAKEUP), \
		"Deep-sleep wake input requires an active-low RTC GPIO with RTC pull-up")

ASSERT_RTC_WAKE_INPUT(DT_ALIAS(sw0), gpios);
ASSERT_RTC_WAKE_INPUT(DT_ALIAS(sw1), gpios);
ASSERT_RTC_WAKE_INPUT(DT_NODELABEL(m5pm1), interrupt_gpios);
ASSERT_RTC_WAKE_INPUT(DT_NODELABEL(papermono_touch), int_gpios);
static const struct pwm_dt_spec buzzer =
	PWM_DT_SPEC_GET(DT_NODELABEL(buzzer));
static enum papermono_wakeup_cause cached_wakeup_cause;
static bool wakeup_cause_cached;
#if defined(CONFIG_PAPERMONO_PDM_MIC)
static const struct device *const microphone =
	DEVICE_DT_GET(DT_ALIAS(microphone0));
#endif
#if defined(CONFIG_PAPERMONO_ST25R3916)
static const struct device *const nfc = DEVICE_DT_GET(DT_NODELABEL(nfc));
#endif

static const struct sensor_trigger motion_trigger = {
	.type = SENSOR_TRIG_MOTION,
	.chan = SENSOR_CHAN_ACCEL_XYZ,
};

static void papermono_motion_wake_handler(
	const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);
}

static int papermono_power_prepare_peripherals(void)
{
	int ret = 0;
	const int led_ret = papermono_status_led_prepare_sleep();

	if (led_ret < 0 && led_ret != -ENOTSUP && led_ret != -ENODEV) {
		LOG_WRN("Unable to turn status LED off for low power (%d)", led_ret);
		/* Do not enter a nominal low-power state while a status channel is
		 * still powered. The interactive caller resumes the status policy and
		 * restores the page when this preparation error is returned.
		 */
		ret = led_ret;
	}

	if (device_is_ready(display)) {
		ret = ret ?: display_set_brightness(display, 0U);
		ret = ret ?: papermono_ssd1677_prepare_poweroff(display);
	}
	ret = ret ?: pwm_set_pulse_dt(&buzzer, 0U);
#if defined(CONFIG_PAPERMONO_ST25R3916)
	if (device_is_ready(nfc)) {
		/* Power-off is idempotent and also stops any active RF field. */
		ret = ret ?: papermono_st25r3916_set_power(nfc, false);
	}
#endif
#if defined(CONFIG_PAPERMONO_PDM_MIC)
	if (device_is_ready(microphone)) {
		ret = ret ?: papermono_pdm_mic_stop(microphone);
	}
#endif
	return ret;
}

static int papermono_power_enable_mfd_idle_sleep(void)
{
	int ret;

	ret = m5ioe1_set_i2c_wake_edge(ioe, M5IOE1_I2C_WAKE_RISING);
	ret = ret ?: m5ioe1_set_i2c_sleep_timeout(ioe, 10U);
	ret = ret ?: m5pm1_set_i2c_sleep_timeout(pmic, 10U);
	return ret;
}

static int papermono_power_prepare_pmic_gpio(uint8_t pin,
				     enum m5pm1_wake_edge edge,
				     gpio_flags_t pull)
{
	uint8_t wake_source;
	int ret;

	ret = m5pm1_configure_wake_gpio(
		pmic, pin, edge, false);
	ret = ret ?: gpio_pin_interrupt_configure(
		pmic, pin, GPIO_INT_DISABLE);
	ret = ret ?: gpio_pin_configure(pmic, pin, GPIO_INPUT | pull);
	ret = ret ?: m5pm1_read_wake_source(pmic, &wake_source, true);
	ret = ret ?: gpio_pin_interrupt_configure(
		pmic, pin, edge == M5PM1_WAKE_RISING ?
			GPIO_INT_EDGE_RISING : GPIO_INT_EDGE_FALLING);
	return ret;
}

#if defined(CONFIG_PAPERMONO_RX8130_TIMER)
static int papermono_power_wait_rtc_wake_line_idle(void)
{
	bool timer;
	bool alarm;
	int level;
	int ret;

	for (uint8_t attempt = 0U; attempt < 60U; ++attempt) {
		ret = papermono_rx8130_clear_irq_status(true, false);
		ret = ret ?: papermono_rx8130_get_irq_status(&timer, &alarm);
		if (ret < 0) {
			return ret;
		}
		ARG_UNUSED(alarm);
		level = gpio_pin_get(pmic, 0U);
		if (level < 0) {
			return level;
		}
		if (level != 0 && !timer) {
			return 0;
		}
		k_msleep(50);
	}
	return -EBUSY;
}
#endif

static int papermono_power_issue_shutdown(void)
{
	int ret;

	ret = papermono_power_prepare_peripherals();
	ret = ret ?: m5pm1_system_command(pmic, M5PM1_SYSTEM_SHUTDOWN);
	if (ret < 0) {
		return ret;
	}
	k_msleep(500);
	return -EIO;
}

int papermono_power_get_wakeup_cause(enum papermono_wakeup_cause *cause)
{
	uint32_t causes;
	uint8_t pmic_cause;
	uint8_t pmic_gpio_mask;
	esp_sleep_wakeup_cause_t esp_cause;
	int ret;

	if (cause == NULL) {
		return -EINVAL;
	}
	if (wakeup_cause_cached) {
		*cause = cached_wakeup_cause;
		return 0;
	}
	causes = esp_sleep_get_wakeup_causes();
	esp_cause = causes == 0U ? ESP_SLEEP_WAKEUP_UNDEFINED :
		(esp_sleep_wakeup_cause_t)__builtin_ctz(causes);
	switch (esp_cause) {
	case ESP_SLEEP_WAKEUP_UNDEFINED:
		ret = m5pm1_get_boot_wake_source(pmic, &pmic_cause);
		if (ret < 0 || pmic_cause == 0U) {
			*cause = PAPERMONO_WAKE_COLD_BOOT;
		} else if ((pmic_cause & BIT(0)) != 0U) {
			*cause = PAPERMONO_WAKE_PMIC_TIMER;
		} else if ((pmic_cause & (BIT(2) | BIT(3))) != 0U) {
			*cause = PAPERMONO_WAKE_PMIC_BUTTON;
		} else if ((pmic_cause & BIT(5)) != 0U) {
			ret = m5pm1_get_boot_wake_gpio_mask(
				pmic, &pmic_gpio_mask);
			*cause = ret == 0 && (pmic_gpio_mask & BIT(0)) != 0U ?
				PAPERMONO_WAKE_PMIC_TIMER :
				PAPERMONO_WAKE_PMIC_EXTERNAL;
		} else {
			*cause = PAPERMONO_WAKE_OTHER;
		}
		break;
	case ESP_SLEEP_WAKEUP_EXT0:
	case ESP_SLEEP_WAKEUP_GPIO:
		*cause = PAPERMONO_WAKE_TOUCH;
		break;
	case ESP_SLEEP_WAKEUP_TIMER:
		*cause = PAPERMONO_WAKE_ESP_TIMER;
		break;
	case ESP_SLEEP_WAKEUP_EXT1: {
		const uint64_t pins = esp_sleep_get_ext1_wakeup_status();

		*cause = papermono_power_classify_ext1(
			pins, BUTTON_WAKE_MASK, PMIC_WAKE_MASK);
		LOG_INF("EXT1 wake GPIO mask 0x%llx (%s)",
			(unsigned long long)pins, papermono_wakeup_cause_name(*cause));
		break;
	}
	default:
		*cause = PAPERMONO_WAKE_OTHER;
		break;
	}
	cached_wakeup_cause = *cause;
	wakeup_cause_cached = true;
	return 0;
}

int papermono_power_acknowledge_wakeup(enum papermono_wakeup_cause cause)
{
	int ret = 0;

	if (cause == PAPERMONO_WAKE_PMIC_TIMER) {
		ret = m5pm1_configure_wake_gpio(
			pmic, 0U, M5PM1_WAKE_FALLING, false);
#if defined(CONFIG_PAPERMONO_RX8130_TIMER)
		ret = ret ?: papermono_rx8130_disable_timer_irq();
		ret = ret ?: papermono_rx8130_clear_irq_status(true, false);
#endif
	} else if (cause == PAPERMONO_WAKE_PMIC_EXTERNAL) {
		ret = m5pm1_configure_wake_gpio(
			pmic, 4U, M5PM1_WAKE_RISING, false);
	}
	return ret;
}

int papermono_power_enter_timed_deep_sleep(uint32_t seconds)
{
	esp_err_t err;
	int ret;

	if (seconds == 0U) {
		return -EINVAL;
	}
	ret = papermono_power_prepare_peripherals();
	if (ret < 0) {
		return ret;
	}
	err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
	if (err == ESP_OK) {
		err = esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000U);
	}
	if (err != ESP_OK) {
		return -EIO;
	}
	ret = papermono_power_enable_mfd_idle_sleep();
	if (ret < 0) {
		return ret;
	}
	LOG_INF("Entering ESP32-S3 deep sleep for %u seconds", seconds);
	k_msleep(20);
	sys_poweroff();
	CODE_UNREACHABLE;
}

static int papermono_power_wait_inputs_idle(
	const struct gpio_dt_spec *const inputs[], size_t count, uint32_t timeout_ms)
{
	const uint32_t started = k_uptime_get_32();

	for (size_t i = 0U; i < count; ++i) {
		if (!gpio_is_ready_dt(inputs[i])) {
			return -ENODEV;
		}
	}
	/* Drivers own these live inputs. A cancelled sleep must not change their
	 * interrupt configuration. Logical active-low readings must all be idle.
	 */
	for (;;) {
		bool idle = true;

		for (size_t i = 0U; i < count; ++i) {
			const int asserted = gpio_pin_get_dt(inputs[i]);

			if (asserted < 0) {
				return asserted;
			}
			idle = idle && asserted == 0;
		}
		if (idle) {
			return 0;
		}
		if (k_uptime_get_32() - started >= timeout_ms) {
			return -EBUSY;
		}
		k_msleep(20);
	}
}

int papermono_power_touch_wake_ready(uint32_t timeout_ms)
{
	const struct gpio_dt_spec *const inputs[] = { &touch_irq };

	return papermono_power_wait_inputs_idle(inputs, ARRAY_SIZE(inputs), timeout_ms);
}

int papermono_power_input_wake_ready(uint32_t timeout_ms)
{
	const struct gpio_dt_spec *const inputs[] = {
		&touch_irq, &button_left, &button_right, &pmic_irq,
	};

	return papermono_power_wait_inputs_idle(inputs, ARRAY_SIZE(inputs), timeout_ms);
}

int papermono_power_enter_touch_deep_sleep(void)
{
	esp_err_t err;
	int ret;

	if (!device_is_ready(pmic)) {
		return -ENODEV;
	}
	ret = m5pm1_prepare_button_wake(pmic);
	ret = ret ?: m5pm1_clear_irq_status(pmic);
	ret = ret ?: papermono_power_input_wake_ready(3000U);
	if (ret < 0) {
		return ret;
	}
	ret = papermono_power_prepare_peripherals();
	if (ret < 0) {
		return ret;
	}
	err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
	if (err == ESP_OK) {
		err = esp_sleep_enable_ext0_wakeup(
			(gpio_num_t)touch_irq.pin, 0);
	}
	if (err == ESP_OK) {
		err = esp_sleep_enable_ext1_wakeup_io(
			BUTTON_WAKE_MASK | PMIC_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
	}
	if (err != ESP_OK) {
		return -EIO;
	}
	ret = papermono_power_enable_mfd_idle_sleep();
	if (ret < 0) {
		return ret;
	}
	LOG_INF("Entering ESP32-S3 deep sleep; touch GPIO%u, side keys GPIO%u/%u, "
		"power-button IRQ GPIO%u armed", touch_irq.pin,
		button_left.pin, button_right.pin, pmic_irq.pin);
	k_msleep(20);
	sys_poweroff();
	CODE_UNREACHABLE;
}

int papermono_power_shutdown_button(void)
{
	int ret;

	ret = m5pm1_configure_wake_gpio(
		pmic, 0U, M5PM1_WAKE_FALLING, false);
	ret = ret ?: m5pm1_configure_wake_gpio(
		pmic, 4U, M5PM1_WAKE_RISING, false);
	ret = ret ?: m5pm1_set_power_hold(pmic, M5PM1_HOLD_LDO, false);
	return ret ?: papermono_power_issue_shutdown();
}

int papermono_power_shutdown_rtc(uint32_t delay_ms, uint32_t *actual_ms)
{
#if !defined(CONFIG_PAPERMONO_RX8130_TIMER)
	ARG_UNUSED(delay_ms);
	ARG_UNUSED(actual_ms);
	return -ENOTSUP;
#else
	uint8_t wake_source;
	int level;
	int ret;

	if (actual_ms == NULL || delay_ms < 1000U) {
		return -EINVAL;
	}
	ret = papermono_rx8130_disable_timer_irq();
	ret = ret ?: papermono_rx8130_clear_irq_status(true, false);
	ret = ret ?: m5pm1_configure_wake_gpio(
		pmic, 4U, M5PM1_WAKE_RISING, false);
	ret = ret ?: gpio_pin_interrupt_configure(
		pmic, 2U, GPIO_INT_DISABLE);
	ret = ret ?: gpio_pin_interrupt_configure(
		pmic, 3U, GPIO_INT_DISABLE);
	ret = ret ?: gpio_pin_interrupt_configure(
		pmic, 4U, GPIO_INT_DISABLE);
	ret = ret ?: m5pm1_clear_irq_status(pmic);
	ret = ret ?: papermono_power_prepare_pmic_gpio(
		0U, M5PM1_WAKE_FALLING, GPIO_PULL_UP);
	ret = ret ?: papermono_power_wait_rtc_wake_line_idle();
	ret = ret ?: papermono_rx8130_set_timer_irq(delay_ms, actual_ms);
	ret = ret ?: m5pm1_clear_irq_status(pmic);
	ret = ret ?: m5pm1_read_wake_source(pmic, &wake_source, true);
	ret = ret ?: papermono_power_wait_rtc_wake_line_idle();
	ret = ret ?: m5pm1_configure_wake_gpio(
		pmic, 0U, M5PM1_WAKE_FALLING, true);
	ret = ret ?: papermono_power_wait_rtc_wake_line_idle();
	if (ret < 0) {
		return ret;
	}
	level = gpio_pin_get(pmic, 0U);
	if (level < 0) {
		return level;
	}
	if (level == 0) {
		(void)m5pm1_configure_wake_gpio(
			pmic, 0U, M5PM1_WAKE_FALLING, false);
		(void)papermono_rx8130_disable_timer_irq();
		return -EBUSY;
	}
	ret = m5pm1_set_power_hold(pmic, M5PM1_HOLD_LDO, false);
	if (ret == 0) {
		LOG_INF("M5PM1 shutdown with RX8130 wake in %u ms", *actual_ms);
	}
	return ret ?: papermono_power_issue_shutdown();
#endif
}

int papermono_power_shutdown_imu(void)
{
	struct sensor_value duration = { .val1 = 100 };
	struct sensor_value threshold = { .val2 = 100000 };
	int level;
	int ret;

	ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SLOPE_DUR, &duration);
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				     SENSOR_ATTR_SLOPE_TH, &threshold);
	ret = ret ?: sensor_trigger_set(
		imu, &motion_trigger, papermono_motion_wake_handler);
	ret = ret ?: m5pm1_configure_wake_gpio(
		pmic, 0U, M5PM1_WAKE_FALLING, false);
	ret = ret ?: papermono_power_prepare_pmic_gpio(
		4U, M5PM1_WAKE_RISING, GPIO_PULL_DOWN);
	ret = ret ?: m5pm1_update_power(
		pmic, M5PM1_POWER_LDO_ENABLE, M5PM1_POWER_LDO_ENABLE);
	ret = ret ?: m5pm1_set_power_hold(pmic, M5PM1_HOLD_LDO, true);
	if (ret < 0) {
		return ret;
	}
	level = gpio_pin_get(pmic, 4U);
	if (level < 0) {
		return level;
	}
	if (level != 0) {
		(void)m5pm1_configure_wake_gpio(
			pmic, 4U, M5PM1_WAKE_RISING, false);
		return -EBUSY;
	}
	ret = m5pm1_configure_wake_gpio(
		pmic, 4U, M5PM1_WAKE_RISING, true);
	if (ret < 0) {
		return ret;
	}
	level = gpio_pin_get(pmic, 4U);
	if (level < 0) {
		return level;
	}
	if (level != 0) {
		(void)m5pm1_configure_wake_gpio(
			pmic, 4U, M5PM1_WAKE_RISING, false);
		return -EBUSY;
	}
	LOG_INF("M5PM1 shutdown with BMI270 any-motion wake");
	return papermono_power_issue_shutdown();
}
