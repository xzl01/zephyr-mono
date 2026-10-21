/* SPDX-License-Identifier: GPL-3.0-only */

#define DT_DRV_COMPAT m5stack_papermono_rgb_led

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <papermono/m5pm1.h>

LOG_MODULE_REGISTER(papermono_rgb, LOG_LEVEL_INF);

struct papermono_rgb_config {
	const struct device *pmic;
	struct pwm_dt_spec green;
	struct pwm_dt_spec blue;
};

struct papermono_rgb_data {
	struct k_mutex lock;
	uint8_t color[3];
	uint8_t brightness;
};

static const uint8_t papermono_rgb_mapping[] = {
	LED_COLOR_ID_RED,
	LED_COLOR_ID_GREEN,
	LED_COLOR_ID_BLUE,
};

static const struct led_info papermono_rgb_info = {
	.label = "status-rgb",
	.index = 0U,
	.num_colors = ARRAY_SIZE(papermono_rgb_mapping),
	.color_mapping = papermono_rgb_mapping,
};

static int papermono_rgb_apply(const struct device *dev)
{
	const struct papermono_rgb_config *config = dev->config;
	struct papermono_rgb_data *data = dev->data;
	uint32_t green;
	uint32_t blue;
	uint8_t red;
	int ret;

	red = (uint16_t)data->color[0] * data->brightness /
		LED_BRIGHTNESS_MAX;
	green = (uint64_t)config->green.period * data->color[1] *
		data->brightness / (UINT8_MAX * LED_BRIGHTNESS_MAX);
	blue = (uint64_t)config->blue.period * data->color[2] *
		data->brightness / (UINT8_MAX * LED_BRIGHTNESS_MAX);

	ret = m5pm1_set_status_led(config->pmic, red != 0U, false);
	if (ret < 0) {
		LOG_ERR("red status LED update failed (%d)", ret);
		return ret;
	}
	ret = pwm_set_pulse_dt(&config->green, green);
	if (ret < 0) {
		LOG_ERR("green PWM update failed (%d)", ret);
		return ret;
	}
	ret = pwm_set_pulse_dt(&config->blue, blue);
	if (ret < 0) {
		LOG_ERR("blue PWM update failed (%d)", ret);
	}
	return ret;
}

static int papermono_rgb_get_info(const struct device *dev, uint32_t led,
				  const struct led_info **info)
{
	ARG_UNUSED(dev);
	if (led != 0U || info == NULL) {
		return -EINVAL;
	}
	*info = &papermono_rgb_info;
	return 0;
}

static int papermono_rgb_set_brightness(const struct device *dev,
					uint32_t led, uint8_t value)
{
	struct papermono_rgb_data *data = dev->data;
	int ret;

	if (led != 0U || value > LED_BRIGHTNESS_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	data->brightness = value;
	ret = papermono_rgb_apply(dev);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_rgb_set_color(const struct device *dev, uint32_t led,
				   uint8_t num_colors, const uint8_t *color)
{
	struct papermono_rgb_data *data = dev->data;
	int ret;

	if (led != 0U || num_colors != ARRAY_SIZE(data->color) || color == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	memcpy(data->color, color, sizeof(data->color));
	ret = papermono_rgb_apply(dev);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_rgb_on(const struct device *dev, uint32_t led)
{
	return papermono_rgb_set_brightness(dev, led, LED_BRIGHTNESS_MAX);
}

static int papermono_rgb_off(const struct device *dev, uint32_t led)
{
	return papermono_rgb_set_brightness(dev, led, 0U);
}

static DEVICE_API(led, papermono_rgb_api) = {
	.on = papermono_rgb_on,
	.off = papermono_rgb_off,
	.set_brightness = papermono_rgb_set_brightness,
	.get_info = papermono_rgb_get_info,
	.set_color = papermono_rgb_set_color,
};

static int papermono_rgb_init(const struct device *dev)
{
	const struct papermono_rgb_config *config = dev->config;
	struct papermono_rgb_data *data = dev->data;

	if (!device_is_ready(config->pmic) || !pwm_is_ready_dt(&config->green) ||
	    !pwm_is_ready_dt(&config->blue)) {
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	data->brightness = LED_BRIGHTNESS_MAX;
	return papermono_rgb_apply(dev);
}

#define PAPERMONO_RGB_DEFINE(inst)                                           \
	static const struct papermono_rgb_config papermono_rgb_config_##inst = { \
		.pmic = DEVICE_DT_GET(DT_INST_PHANDLE(inst, pmic)),               \
		.green = PWM_DT_SPEC_INST_GET_BY_NAME(inst, green),               \
		.blue = PWM_DT_SPEC_INST_GET_BY_NAME(inst, blue),                 \
	};                                                                         \
	static struct papermono_rgb_data papermono_rgb_data_##inst;                \
	DEVICE_DT_INST_DEFINE(inst, papermono_rgb_init, NULL,                       \
			      &papermono_rgb_data_##inst, &papermono_rgb_config_##inst, \
			      POST_KERNEL, CONFIG_PAPERMONO_BOARD_FUNCTION_INIT_PRIORITY, \
			      &papermono_rgb_api);

DT_INST_FOREACH_STATUS_OKAY(PAPERMONO_RGB_DEFINE)
