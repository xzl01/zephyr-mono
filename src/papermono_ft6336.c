/* SPDX-License-Identifier: GPL-3.0-only */

#define DT_DRV_COMPAT focaltech_ft5336

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <papermono/ft6336.h>
#include <papermono/touch_transform.h>

LOG_MODULE_REGISTER(papermono_ft6336, CONFIG_INPUT_LOG_LEVEL);

#define FT6336_REG_DEVICE_MODE 0x00U
#define FT6336_REG_TOUCH_DATA  0x02U
#define FT6336_REG_CHIP_ID     0xA3U
#define FT6336_REG_G_MODE      0xA4U

#define FT6336_TOUCH_POINTS_MASK 0x0FU
#define FT6336_POSITION_HIGH_MASK 0x0FU
#define FT6336_RESET_LOW_MS 8U
#define FT6336_RESET_SETTLE_MS 10U
#define FT6336_PROBE_ATTEMPTS 12U
#define FT6336_PROBE_RETRY_MS 50U
#define FT6336_REPROBE_MS 250U
#define FT6336_STALE_RELEASE_MS 100U
#define FT6336_OFFLINE_ERRORS 10U

#define PAPERMONO_DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define FT6336_SCREEN_WIDTH DT_PROP(PAPERMONO_DISPLAY_NODE, width)
#define FT6336_SCREEN_HEIGHT DT_PROP(PAPERMONO_DISPLAY_NODE, height)

BUILD_ASSERT(FT6336_SCREEN_WIDTH == 480U && FT6336_SCREEN_HEIGHT == 800U,
	"Paper Mono touch transform requires the 480x800 portrait display");

struct papermono_ft6336_config {
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec bus;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec int_gpio;
};

struct papermono_ft6336_data {
	const struct device *dev;
	struct k_thread thread;
	K_KERNEL_STACK_MEMBER(thread_stack,
		CONFIG_PAPERMONO_FT6336_THREAD_STACK_SIZE);
	atomic_t online;
	atomic_t pressed;
	atomic_t frames;
	atomic_t press_events;
	atomic_t release_events;
	atomic_t read_errors;
	atomic_t invalid_frames;
	atomic_t recoveries;
	atomic_t last_poll_ms;
	atomic_t max_poll_gap_ms;
	atomic_t last_sample_ms;
	atomic_t last_x;
	atomic_t last_y;
	uint32_t next_probe_ms;
	uint8_t consecutive_errors;
};

INPUT_TOUCH_STRUCT_CHECK(struct papermono_ft6336_config);

static const struct device *touch_device;

static int papermono_ft6336_prepare_once(const struct device *dev,
	uint8_t chip_id[6])
{
	const struct papermono_ft6336_config *config = dev->config;
	int ret;

	ret = i2c_reg_write_byte_dt(&config->bus, FT6336_REG_DEVICE_MODE, 0U);
	ret = ret ?: i2c_burst_read_dt(&config->bus, FT6336_REG_CHIP_ID,
		chip_id, 6U);
	/* G_MODE=0 is the controller's polling/periodic-report mode. */
	ret = ret ?: i2c_reg_write_byte_dt(&config->bus, FT6336_REG_G_MODE, 0U);
	return ret;
}

static int papermono_ft6336_prepare(const struct device *dev)
{
	uint8_t chip_id[6] = { 0 };
	int ret = -EIO;

	/* PaperMono units can legitimately return an all-zero identification
	 * window. Transaction success, rather than the ID bytes, is authoritative.
	 */
	for (uint8_t attempt = 0U; attempt < FT6336_PROBE_ATTEMPTS; ++attempt) {
		if (attempt != 0U) {
			k_msleep(FT6336_PROBE_RETRY_MS);
		}
		ret = papermono_ft6336_prepare_once(dev, chip_id);
		if (ret == 0) {
			LOG_INF("FT6336 ready after %u attempt(s); id=%02x/%02x/%02x",
				attempt + 1U, chip_id[0], chip_id[3], chip_id[5]);
			return 0;
		}
	}
	return ret;
}

static void papermono_ft6336_report_release(
	const struct device *dev, struct papermono_ft6336_data *data)
{
	if (atomic_cas(&data->pressed, 1, 0)) {
		atomic_inc(&data->release_events);
		const int ret = input_report_key(dev, INPUT_BTN_TOUCH, 0, true,
			K_FOREVER);

		if (ret < 0) {
			LOG_WRN("FT6336 release event dropped (%d)", ret);
		} else {
			/* Coordinates can reveal on-device password keystrokes. */
			LOG_DBG("FT6336 touch released");
		}
	}
}

static int papermono_ft6336_process(const struct device *dev)
{
	const struct papermono_ft6336_config *config = dev->config;
	struct papermono_ft6336_data *data = dev->data;
	const uint32_t now = k_uptime_get_32();
	uint8_t frame[6] = { 0 };
	uint16_t x;
	uint16_t y;
	struct papermono_touch_point point;
	int ret;
	const uint32_t previous_poll = (uint32_t)atomic_set(&data->last_poll_ms,
		now);

	if (previous_poll != 0U) {
		const atomic_val_t gap = (atomic_val_t)(now - previous_poll);
		atomic_val_t maximum = atomic_get(&data->max_poll_gap_ms);

		while (gap > maximum &&
		       !atomic_cas(&data->max_poll_gap_ms, maximum, gap)) {
			maximum = atomic_get(&data->max_poll_gap_ms);
		}
	}

	if (atomic_get(&data->online) == 0) {
		/* Ten fast I2C failures mark the controller offline before the
		 * stale-release timeout expires. Do not leave BTN_TOUCH latched while
		 * waiting for the slower background reprobe interval.
		 */
		if (atomic_get(&data->pressed) != 0 &&
		    now - (uint32_t)atomic_get(&data->last_sample_ms) >=
			FT6336_STALE_RELEASE_MS) {
			papermono_ft6336_report_release(dev, data);
		}
		if ((int32_t)(now - data->next_probe_ms) < 0) {
			return -EAGAIN;
		}
		ret = papermono_ft6336_prepare_once(dev, frame);
		if (ret < 0) {
			data->next_probe_ms = now + FT6336_REPROBE_MS;
			atomic_inc(&data->read_errors);
			return ret;
		}
		atomic_set(&data->online, 1);
		atomic_inc(&data->recoveries);
		data->consecutive_errors = 0U;
		LOG_INF("FT6336 recovered without a system restart");
	}

	/* Read TD_STATUS and the complete first contact in one transaction so a
	 * report cannot be assembled from two different controller frames.
	 */
	ret = i2c_burst_read_dt(&config->bus, FT6336_REG_TOUCH_DATA,
		frame, 5U);
	if (ret < 0) {
		atomic_inc(&data->read_errors);
		data->consecutive_errors++;
		if (atomic_get(&data->pressed) != 0 &&
		    now - (uint32_t)atomic_get(&data->last_sample_ms) >=
			FT6336_STALE_RELEASE_MS) {
			papermono_ft6336_report_release(dev, data);
		}
		if (data->consecutive_errors >= FT6336_OFFLINE_ERRORS) {
			atomic_clear(&data->online);
			data->next_probe_ms = now + FT6336_REPROBE_MS;
			data->consecutive_errors = 0U;
		}
		return ret;
	}

	data->consecutive_errors = 0U;
	atomic_inc(&data->frames);
	if ((frame[0] & FT6336_TOUCH_POINTS_MASK) == 0U) {
		atomic_set(&data->last_sample_ms, now);
		papermono_ft6336_report_release(dev, data);
		return 0;
	}

	x = ((uint16_t)(frame[1] & FT6336_POSITION_HIGH_MASK) << 8U) |
		frame[2];
	y = ((uint16_t)(frame[3] & FT6336_POSITION_HIGH_MASK) << 8U) |
		frame[4];
	ret = papermono_touch_map_portrait(x, y, FT6336_SCREEN_WIDTH,
		FT6336_SCREEN_HEIGHT, &point);
	if (ret < 0) {
		atomic_inc(&data->invalid_frames);
		if (atomic_get(&data->pressed) != 0 &&
		    now - (uint32_t)atomic_get(&data->last_sample_ms) >=
			FT6336_STALE_RELEASE_MS) {
			papermono_ft6336_report_release(dev, data);
		}
		return ret;
	}
	atomic_set(&data->last_sample_ms, now);
	input_touchscreen_report_pos(dev, point.x, point.y, K_FOREVER);
	atomic_set(&data->last_x, point.x);
	atomic_set(&data->last_y, point.y);
	/* Position reports continue while held, but BTN_TOUCH is an edge. Repeating
	 * the press key on every poll would restart the UI's gesture timer and turn
	 * every swipe or long press into a short tap.
	 */
	if (atomic_cas(&data->pressed, 0, 1)) {
		atomic_inc(&data->press_events);
		ret = input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
		if (ret < 0) {
			LOG_WRN("FT6336 press event dropped (%d)", ret);
		} else {
			LOG_DBG("FT6336 touch pressed");
		}
	}
	return 0;
}

static void papermono_ft6336_thread(void *arg1, void *arg2, void *arg3)
{
	struct papermono_ft6336_data *data = arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	for (;;) {
		(void)papermono_ft6336_process(data->dev);
		k_msleep(CONFIG_PAPERMONO_FT6336_POLL_INTERVAL_MS);
	}
}

static int papermono_ft6336_init(const struct device *dev)
{
	const struct papermono_ft6336_config *config = dev->config;
	struct papermono_ft6336_data *data = dev->data;
	int ret;

	if (!i2c_is_ready_dt(&config->bus) ||
	    !gpio_is_ready_dt(&config->reset_gpio) ||
	    !gpio_is_ready_dt(&config->int_gpio)) {
		return -ENODEV;
	}

	data->dev = dev;

	/* GPIO4 remains the active-low deep-sleep wake source even though normal
	 * touch sampling is polling-based. Configure its RTC pull-up here; the
	 * generic Zephyr polling driver never touches int-gpios.
	 */
	ret = gpio_pin_configure_dt(&config->int_gpio,
		GPIO_INPUT | GPIO_PULL_UP | GPIO_INT_WAKEUP);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	k_msleep(FT6336_RESET_LOW_MS);
	ret = gpio_pin_set_dt(&config->reset_gpio, 0);
	if (ret < 0) {
		return ret;
	}
	k_msleep(FT6336_RESET_SETTLE_MS);

	ret = papermono_ft6336_prepare(dev);
	if (ret < 0) {
		LOG_WRN("FT6336 did not answer the boot probe (%d); background recovery armed",
			ret);
		data->next_probe_ms = k_uptime_get_32() + FT6336_REPROBE_MS;
	} else {
		atomic_set(&data->online, 1);
	}
	touch_device = dev;
	(void)k_thread_create(&data->thread, data->thread_stack,
		K_KERNEL_STACK_SIZEOF(data->thread_stack), papermono_ft6336_thread,
		data, NULL, NULL,
		K_PRIO_PREEMPT(CONFIG_PAPERMONO_FT6336_THREAD_PRIORITY), 0,
		K_MSEC(CONFIG_PAPERMONO_FT6336_POLL_INTERVAL_MS));
	(void)k_thread_name_set(&data->thread, "papermono_touch");
	LOG_INF("FT6336 dedicated polling thread: %d ms, %d-byte stack",
		CONFIG_PAPERMONO_FT6336_POLL_INTERVAL_MS,
		CONFIG_PAPERMONO_FT6336_THREAD_STACK_SIZE);
	return 0;
}

int papermono_ft6336_get_status(struct papermono_ft6336_status *status)
{
	struct papermono_ft6336_data *data;

	if (status == NULL) {
		return -EINVAL;
	}
	if (touch_device == NULL || !device_is_ready(touch_device)) {
		return -ENODEV;
	}
	data = touch_device->data;
	*status = (struct papermono_ft6336_status) {
		.frames = (uint32_t)atomic_get(&data->frames),
		.press_events = (uint32_t)atomic_get(&data->press_events),
		.release_events = (uint32_t)atomic_get(&data->release_events),
		.read_errors = (uint32_t)atomic_get(&data->read_errors),
		.invalid_frames = (uint32_t)atomic_get(&data->invalid_frames),
		.recoveries = (uint32_t)atomic_get(&data->recoveries),
		.last_sample_ms = (uint32_t)atomic_get(&data->last_sample_ms),
		.max_poll_gap_ms = (uint32_t)atomic_get(&data->max_poll_gap_ms),
		.x = atomic_get(&data->last_x),
		.y = atomic_get(&data->last_y),
		.online = atomic_get(&data->online) != 0,
		.pressed = atomic_get(&data->pressed) != 0,
	};
	return 0;
}

#define PAPERMONO_FT6336_DEFINE(inst)                                           \
	static const struct papermono_ft6336_config ft6336_config_##inst = {       \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(inst),              \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                    \
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),               \
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                   \
	};                                                                            \
	static struct papermono_ft6336_data ft6336_data_##inst;                       \
	DEVICE_DT_INST_DEFINE(inst, papermono_ft6336_init, NULL,                      \
		&ft6336_data_##inst, &ft6336_config_##inst, POST_KERNEL,              \
		CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAPERMONO_FT6336_DEFINE)
