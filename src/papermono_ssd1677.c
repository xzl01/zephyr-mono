/*
 * Copyright (c) 2026 M5Stack Technology CO LTD
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <papermono/m5ioe1.h>
#include <papermono/refresh_policy.h>
#include <papermono/ssd1677.h>

LOG_MODULE_REGISTER(papermono_ssd1677, CONFIG_DISPLAY_LOG_LEVEL);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

#define VISIBLE_WIDTH  480U
#define VISIBLE_HEIGHT 800U
#define RAW_WIDTH      VISIBLE_HEIGHT
#define RAW_HEIGHT     VISIBLE_WIDTH
#define RAW_ROW_BYTES  (RAW_WIDTH / 8U)
#define FRAME_SIZE     (RAW_ROW_BYTES * RAW_HEIGHT)
#define SPI_CHUNK_SIZE 4092U
#define BUSY_TIMEOUT   K_SECONDS(15)

#define SSD1677_CMD_DRIVER_OUTPUT_CONTROL 0x01U
#define SSD1677_CMD_BOOSTER_SOFT_START    0x0CU
#define SSD1677_CMD_DEEP_SLEEP           0x10U
#define SSD1677_CMD_DATA_ENTRY_MODE       0x11U
#define SSD1677_CMD_SOFT_RESET            0x12U
#define SSD1677_CMD_TEMP_SENSOR_CONTROL   0x18U
#define SSD1677_CMD_TEMP_SENSOR_WRITE     0x1AU
#define SSD1677_CMD_MASTER_ACTIVATION     0x20U
#define SSD1677_CMD_DISPLAY_MODE          0x21U
#define SSD1677_CMD_UPDATE_CONTROL        0x22U
#define SSD1677_CMD_WRITE_RAM_1           0x24U
#define SSD1677_CMD_WRITE_RAM_2           0x26U
#define SSD1677_CMD_BORDER_WAVEFORM       0x3CU
#define SSD1677_CMD_RAM_X_RANGE           0x44U
#define SSD1677_CMD_RAM_Y_RANGE           0x45U
#define SSD1677_CMD_RAM_X_COUNTER         0x4EU
#define SSD1677_CMD_RAM_Y_COUNTER         0x4FU

struct papermono_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec cs;
	struct gpio_dt_spec dc;
	struct gpio_dt_spec busy;
	struct gpio_dt_spec power;
	struct gpio_dt_spec reset;
	struct pwm_dt_spec frontlight;
};

struct papermono_data {
	struct k_mutex lock;
	bool blanking;
	bool dirty;
	bool baseline_ready;
	bool controller_sleeping;
	uint8_t consecutive_partials;
	enum papermono_refresh_quality refresh_hint;
	uint32_t full_refreshes;
	uint32_t partial_refreshes;
	uint32_t partial_fallbacks;
	uint32_t gray_refreshes;
	uint32_t last_full_refresh_ms;
	uint32_t last_partial_refresh_ms;
	uint8_t frame[FRAME_SIZE];
	uint8_t staging[SPI_CHUNK_SIZE];
};

static int papermono_reset_assert(const struct papermono_config *config)
{
	/* Drive RESET physically low.  Reconfigure rather than only updating the
	 * output latch because the released state below deliberately leaves the
	 * M5IOE1 pin in input mode.
	 */
	return gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_ACTIVE);
}

static int papermono_reset_release(const struct papermono_config *config)
{
	/* OUT bit 4 can read back high while the M5IOE1 input sense remains low on
	 * a wedged C153.  Release the output stage completely and use the expander's
	 * pull-up so RESET is electrically high instead of trusting the latch alone.
	 */
	return gpio_pin_configure_dt(&config->reset,
				     GPIO_INPUT | GPIO_PULL_UP);
}

static int papermono_reset_verify_released(
	const struct papermono_config *config, const char *phase)
{
	const int active = gpio_pin_get_dt(&config->reset);

	if (active < 0) {
		return active;
	}
	if (active != 0) {
		LOG_ERR("SSD1677 RESET remained physically low after %s", phase);
		return -EIO;
	}
	return 0;
}

static void papermono_log_panel_lines(
	const struct papermono_config *config, const char *phase)
{
	uint8_t input_bytes[2];
	uint8_t latch_bytes[2];
	uint16_t input = 0U;
	uint16_t latch = 0U;
	const int power = gpio_pin_get_dt(&config->power);
	const int reset = gpio_pin_get_dt(&config->reset);
	const int busy = gpio_pin_get_dt(&config->busy);
	const int input_ret = m5ioe1_reg_read(config->power.port, 0x07U,
					      input_bytes, sizeof(input_bytes));
	const int latch_ret = m5ioe1_reg_read(config->power.port, 0x05U,
					      latch_bytes, sizeof(latch_bytes));

	if (input_ret == 0) {
		input = sys_get_le16(input_bytes);
	}
	if (latch_ret == 0) {
		latch = sys_get_le16(latch_bytes);
	}

	LOG_INF("SSD1677 lines %s: EPD_EN=%d RESET(active)=%d BUSY=%d IN=0x%04x OUT=0x%04x%s",
		phase, power, reset, busy, input, latch,
		(input_ret == 0 && latch_ret == 0) ? "" : " (unreadable)");
}

static int papermono_hardware_power_cycle(
	const struct papermono_config *config, uint32_t off_ms, uint32_t settle_ms)
{
	int ret;

	/* Remove the panel rail only for timeout recovery.  Clamp every directly
	 * controlled input physically low for the entire discharge interval so an
	 * ESP32 output cannot back-power the controller through its input clamp.
	 * Once the rail is restored, make CS/RESET inactive high and issue a complete
	 * high-low-high reset pulse after power has settled.  Starting with RESET low
	 * and merely releasing it does not reproduce the factory sequence.
	 */
	ret = papermono_reset_assert(config);
	ret = ret ?: gpio_pin_set_dt(&config->cs, 1);
	ret = ret ?: gpio_pin_set_dt(&config->dc, 0);
	ret = ret ?: gpio_pin_configure_dt(&config->busy, GPIO_INPUT);
	ret = ret ?: gpio_pin_set_dt(&config->power, 0);
	if (ret < 0) {
		return ret;
	}
	papermono_log_panel_lines(config, "rail-off");
	k_msleep(off_ms);

	ret = gpio_pin_set_dt(&config->power, 1);
	k_msleep(2);
	ret = ret ?: papermono_reset_release(config);
	ret = ret ?: gpio_pin_set_dt(&config->cs, 0);
	k_msleep(settle_ms);
	papermono_log_panel_lines(config, "rail-on/reset-idle");
	ret = ret ?: gpio_pin_configure_dt(&config->busy,
					  GPIO_INPUT | GPIO_PULL_UP);
	ret = ret ?: papermono_reset_assert(config);
	k_msleep(10);
	papermono_log_panel_lines(config, "reset-asserted");
	ret = ret ?: papermono_reset_release(config);
	k_msleep(10);
	papermono_log_panel_lines(config, "reset-released");
	return ret ?: papermono_reset_verify_released(config,
						      "power-cycle release");
}

static int papermono_hardware_reset(const struct papermono_config *config)
{
	int ret;

	/* Match the C153 OTP and M5GFX startup sequence.  A normal hardware reset
	 * keeps EPD_3V3_L3 enabled; cutting it here would erase the controller RAM
	 * baseline required by partial refreshes.
	 */
	ret = gpio_pin_set_dt(&config->cs, 0);
	ret = ret ?: gpio_pin_set_dt(&config->power, 1);
	ret = ret ?: gpio_pin_configure_dt(&config->busy,
					  GPIO_INPUT | GPIO_PULL_UP);
	ret = ret ?: papermono_reset_release(config);
	if (ret < 0) {
		return ret;
	}
	k_msleep(10);
	ret = ret ?: papermono_reset_assert(config);
	if (ret < 0) {
		return ret;
	}
	k_msleep(10);
	ret = papermono_reset_release(config);
	k_msleep(10);
	papermono_log_panel_lines(config, "reset-pulse-complete");
	return ret ?: papermono_reset_verify_released(config,
						      "hardware reset");
}

static int papermono_wait_ready(const struct papermono_config *config,
				const char *phase)
{
	const int64_t deadline = k_uptime_get() + k_ticks_to_ms_floor64(BUSY_TIMEOUT.ticks);
	int value;

	/* M5GFX uses two milliseconds because BUSY may assert shortly after an
	 * activation command; sampling earlier can mistake the pre-activation low
	 * level for completion and corrupt the following command sequence.
	 */
	k_msleep(2);
	for (;;) {
		value = gpio_pin_get_dt(&config->busy);
		if (value < 0) {
			return value;
		}
		if (value == 0) {
			return 0;
		}
		if (k_uptime_get() >= deadline) {
			LOG_ERR("SSD1677 BUSY timeout during %s", phase);
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}
}

static int papermono_begin_write(const struct papermono_config *config)
{
	return gpio_pin_set_dt(&config->cs, 1);
}

static int papermono_end_write(const struct papermono_config *config,
				int transaction_ret)
{
	const int deselect_ret = gpio_pin_set_dt(&config->cs, 0);

	return transaction_ret < 0 ? transaction_ret : deselect_ret;
}

static int papermono_spi_tx(const struct device *dev, const uint8_t *bytes,
			    size_t length, bool data_mode, bool invert)
{
	const struct papermono_config *config = dev->config;
	struct papermono_data *data = dev->data;
	int ret;

	ret = gpio_pin_set_dt(&config->dc, data_mode ? 1 : 0);
	if (ret < 0) {
		return ret;
	}

	while (length > 0U) {
		const size_t chunk = MIN(length, SPI_CHUNK_SIZE);
		const uint8_t *source = bytes;
		struct spi_buf buffer;
		struct spi_buf_set set;

		if (invert) {
			for (size_t i = 0; i < chunk; ++i) {
				data->staging[i] = (uint8_t)~bytes[i];
			}
			source = data->staging;
		}

		buffer.buf = (void *)source;
		buffer.len = chunk;
		set.buffers = &buffer;
		set.count = 1U;
		ret = spi_write_dt(&config->spi, &set);
		if (ret < 0) {
			return ret;
		}

		bytes += chunk;
		length -= chunk;
	}

	return 0;
}

static int papermono_command(const struct device *dev, uint8_t command)
{
	return papermono_spi_tx(dev, &command, sizeof(command), false, false);
}

static int papermono_write_data(const struct device *dev, const uint8_t *bytes,
				size_t length, bool invert)
{
	return papermono_spi_tx(dev, bytes, length, true, invert);
}

static int papermono_register(const struct device *dev, uint8_t command,
			      const uint8_t *bytes, size_t length)
{
	int ret;

	ret = papermono_command(dev, command);
	if (ret == 0 && length > 0U) {
		ret = papermono_write_data(dev, bytes, length, false);
	}

	return ret;
}

static int papermono_u16(const struct device *dev, uint16_t value)
{
	uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};

	return papermono_write_data(dev, bytes, sizeof(bytes), false);
}

static int papermono_set_full_window(const struct device *dev)
{
	static const uint8_t entry_mode = 0x03U;
	int ret;

	ret = papermono_register(dev, SSD1677_CMD_DATA_ENTRY_MODE,
				 &entry_mode, sizeof(entry_mode));
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_X_RANGE);
	ret = ret ?: papermono_u16(dev, 0U);
	ret = ret ?: papermono_u16(dev, RAW_WIDTH - 1U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_Y_RANGE);
	ret = ret ?: papermono_u16(dev, 0U);
	ret = ret ?: papermono_u16(dev, RAW_HEIGHT - 1U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_X_COUNTER);
	ret = ret ?: papermono_u16(dev, 0U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_Y_COUNTER);
	ret = ret ?: papermono_u16(dev, 0U);

	return ret;
}

static int papermono_set_gray_window(const struct device *dev)
{
	static const uint8_t entry_mode = 0x02U;
	int ret;

	/* The built-in gray waveform consumes RAM X from 799 down to zero. */
	ret = papermono_register(dev, SSD1677_CMD_DATA_ENTRY_MODE,
				 &entry_mode, sizeof(entry_mode));
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_X_RANGE);
	ret = ret ?: papermono_u16(dev, RAW_WIDTH - 1U);
	ret = ret ?: papermono_u16(dev, 0U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_Y_RANGE);
	ret = ret ?: papermono_u16(dev, 0U);
	ret = ret ?: papermono_u16(dev, RAW_HEIGHT - 1U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_X_COUNTER);
	ret = ret ?: papermono_u16(dev, RAW_WIDTH - 1U);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_RAM_Y_COUNTER);
	ret = ret ?: papermono_u16(dev, 0U);

	return ret;
}

static int papermono_issue_software_reset(const struct device *dev,
					  const char *phase)
{
	const struct papermono_config *config = dev->config;
	int ret;

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_SOFT_RESET);
	ret = papermono_end_write(config, ret);
	if (ret < 0) {
		return ret;
	}

	k_msleep(10);
	return papermono_wait_ready(config, phase);

}

static int papermono_software_reset(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	int ret;

	ret = papermono_wait_ready(config, "software reset pre-check");
	if (ret < 0) {
		return ret;
	}

	return papermono_issue_software_reset(dev, "software reset");
}

static int papermono_init_mono_mode(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	static const uint8_t temp_sensor[] = {0x80U};
	static const uint8_t soft_start[] = {0xAEU, 0xC7U, 0xC3U, 0xC0U, 0x80U};
	static const uint8_t driver_output[] = {0xDFU, 0x01U, 0x02U};
	static const uint8_t border[] = {0x01U};
	static const uint8_t display_mode[] = {0x00U};
	int ret;

	ret = papermono_software_reset(dev);
	if (ret < 0) {
		return ret;
	}

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_TEMP_SENSOR_CONTROL,
				 temp_sensor, sizeof(temp_sensor));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_BOOSTER_SOFT_START,
					soft_start, sizeof(soft_start));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_DRIVER_OUTPUT_CONTROL,
					driver_output, sizeof(driver_output));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_BORDER_WAVEFORM,
					border, sizeof(border));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_DISPLAY_MODE,
					display_mode, sizeof(display_mode));
	ret = ret ?: papermono_set_full_window(dev);
	ret = papermono_end_write(config, ret);

	return ret;
}

static int papermono_init_gray_mode(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	static const uint8_t soft_start[] = {0xAEU, 0xC7U, 0xC3U, 0xC0U, 0x80U};
	static const uint8_t driver_output[] = {0xDFU, 0x01U, 0x02U};
	static const uint8_t border[] = {0x01U};
	static const uint8_t temp_sensor[] = {0x80U};
	static const uint8_t gray_temperature[] = {0x5AU};
	int ret;

	ret = papermono_software_reset(dev);
	if (ret < 0) {
		return ret;
	}

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_BOOSTER_SOFT_START,
				soft_start, sizeof(soft_start));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_DRIVER_OUTPUT_CONTROL,
					driver_output, sizeof(driver_output));
	ret = ret ?: papermono_set_gray_window(dev);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_BORDER_WAVEFORM,
					border, sizeof(border));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_TEMP_SENSOR_CONTROL,
					temp_sensor, sizeof(temp_sensor));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_TEMP_SENSOR_WRITE,
					gray_temperature,
					sizeof(gray_temperature));
	ret = papermono_end_write(config, ret);
	return ret;
}

static int papermono_stream_gray_plane(
	const struct device *dev, uint8_t command,
	enum papermono_ssd1677_gray_plane plane,
	papermono_ssd1677_gray4_reader_t reader, void *context)
{
	struct papermono_data *data = dev->data;
	size_t offset = 0U;
	int ret;

	ret = papermono_command(dev, command);
	while (ret == 0 && offset < FRAME_SIZE) {
		const size_t length = MIN(FRAME_SIZE - offset, SPI_CHUNK_SIZE);

		ret = reader(context, plane, offset, data->staging, length);
		if (ret == 0) {
			ret = papermono_write_data(dev, data->staging, length,
						   false);
		}
		offset += length;
	}
	return ret;
}

static int papermono_deep_sleep(const struct device *dev,
				bool power_off_required);

int papermono_ssd1677_refresh_gray4(
	const struct device *dev, papermono_ssd1677_gray4_reader_t reader,
	void *context)
{
	const struct papermono_config *config = dev->config;
	struct papermono_data *data = dev->data;
	static const uint8_t gray_control = 0xD7U;
	const uint32_t started_ms = k_uptime_get_32();
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	if (reader == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	LOG_INF("Starting SSD1677 four-gray OTP full refresh");
	data->baseline_ready = false;
	data->controller_sleeping = false;

	ret = papermono_hardware_reset(config);
	ret = ret ?: papermono_init_gray_mode(dev);
	if (ret < 0) {
		goto out;
	}

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_stream_gray_plane(dev, SSD1677_CMD_WRITE_RAM_1,
					  PAPERMONO_SSD1677_GRAY_PLANE_1,
					  reader, context);
	ret = ret ?: papermono_stream_gray_plane(
		dev, SSD1677_CMD_WRITE_RAM_2,
		PAPERMONO_SSD1677_GRAY_PLANE_2, reader, context);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
					&gray_control, sizeof(gray_control));
	ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
	ret = papermono_end_write(config, ret);
	ret = ret ?: papermono_wait_ready(config, "four-gray activation");
	/* Gray control 0xD7 already finishes with analog power and clock off. */
	ret = ret ?: papermono_deep_sleep(dev, false);

out:
	/* A gray update is not a valid monochrome differential baseline. Force
	 * the next LVGL flush to rebuild it with a monochrome full refresh.
	 */
	data->dirty = true;
	data->baseline_ready = false;
	data->consecutive_partials = 0U;
	if (ret == 0) {
		data->gray_refreshes++;
		LOG_INF("SSD1677 four-gray refresh %u complete in %u ms",
			data->gray_refreshes, k_uptime_get_32() - started_ms);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_write_frame(const struct device *dev, uint8_t command,
				 const uint8_t *frame, bool invert)
{
	int ret;

	ret = papermono_set_full_window(dev);
	ret = ret ?: papermono_command(dev, command);
	ret = ret ?: papermono_write_data(dev, frame, FRAME_SIZE, invert);
	return ret;
}

static int papermono_deep_sleep(const struct device *dev,
				bool power_off_required)
{
	const struct papermono_config *config = dev->config;
	static const uint8_t power_off = 0x03U;
	static const uint8_t mode = 0x01U;
	int ret;

	if (power_off_required) {
		/* The two-stage monochrome OTP sequence leaves the analog supply and
		 * clock enabled.  Match the M5GFX power-save sequence before entering
		 * deep sleep.  Gray control 0xD7 and partial control 0xFF already
		 * contain the corresponding power-down bits, so repeating this command
		 * after those activations can leave BUSY asserted indefinitely.
		 */
		ret = papermono_begin_write(config);
		ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
					 &power_off, sizeof(power_off));
		ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
		ret = papermono_end_write(config, ret);
		ret = ret ?: papermono_wait_ready(config, "analog power-off");
		if (ret < 0) {
			return ret;
		}
	}

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(
		dev, SSD1677_CMD_DEEP_SLEEP, &mode, sizeof(mode));
	ret = papermono_end_write(config, ret);
	k_msleep(100);
	if (ret == 0) {
		struct papermono_data *data = dev->data;

		data->controller_sleeping = true;
	}
	return ret;
}

int papermono_ssd1677_prepare_poweroff(const struct device *dev)
{
	const struct papermono_config *config;
	struct papermono_data *data;
	static const uint8_t power_off = 0x03U;
	int busy;
	int graceful_ret = 0;
	int ret;

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	config = dev->config;
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);

	busy = gpio_pin_get_dt(&config->busy);
	if (busy < 0) {
		graceful_ret = busy;
	} else if (busy != 0) {
		/* Do not wait for an activation which has already exceeded the normal
		 * refresh timeout.  Holding reset while removing EINK_EN is the only
		 * deterministic way to discard that controller state.
		 */
		LOG_WRN("SSD1677 BUSY asserted at board power-off; forcing hard power-off");
	} else if (!data->controller_sleeping) {
		graceful_ret = papermono_begin_write(config);
		graceful_ret = graceful_ret ?: papermono_register(
			dev, SSD1677_CMD_UPDATE_CONTROL,
			&power_off, sizeof(power_off));
		graceful_ret = graceful_ret ?:
			papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
		graceful_ret = papermono_end_write(config, graceful_ret);
		graceful_ret = graceful_ret ?:
			papermono_wait_ready(config, "board power-off");
	}

	/* The e-paper keeps its visible image without controller power.  Always
	 * remove EINK_EN before the PMIC command so stale BUSY/analog state cannot
	 * be retained by the always-on M5IOE1 across the next boot.
	 */
	ret = papermono_reset_assert(config);
	ret = ret ?: gpio_pin_set_dt(&config->cs, 1);
	ret = ret ?: gpio_pin_set_dt(&config->dc, 0);
	ret = ret ?: gpio_pin_configure_dt(&config->busy, GPIO_INPUT);
	ret = ret ?: gpio_pin_set_dt(&config->power, 0);
	k_msleep(20);
	data->controller_sleeping = false;
	data->baseline_ready = false;
	data->consecutive_partials = 0U;
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		return ret;
	}
	if (graceful_ret < 0) {
		LOG_WRN("SSD1677 graceful power-off failed (%d); hard power-off completed",
			graceful_ret);
	}
	LOG_INF("SSD1677 controller power removed for board shutdown");
	return 0;
}

static int papermono_refresh_full(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	struct papermono_data *data = dev->data;
#if !defined(CONFIG_PAPERMONO_FULL_REFRESH_COMPACT)
	static const uint8_t synchronize_control = 0xF8U;
	static const uint8_t otp_control = 0x14U;
#endif
	const uint32_t started_ms = k_uptime_get_32();
	int ret;

	LOG_INF("Starting SSD1677 monochrome OTP full refresh");
	data->baseline_ready = false;
	data->controller_sleeping = false;
	ret = papermono_hardware_reset(config);
	ret = ret ?: papermono_init_mono_mode(dev);
	if (ret < 0) {
		return ret;
	}

#if defined(CONFIG_PAPERMONO_FULL_REFRESH_COMPACT)
	/* Single self-contained Mode-1 sequence (0x22 = 0xF4): clock, analog,
	 * temperature, Mode-1 LUT load and DISPLAY Mode 1 in one activation. The
	 * legacy 0xF8 phase has no DISPLAY bit (bit 2), so its RAM_1 write is
	 * overwritten by this phase anyway. Validate on hardware before enabling.
	 */
	static const uint8_t compact_control = 0xF4U;

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_2,
					   data->frame, true);
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_1,
					   data->frame, true);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
				 &compact_control, sizeof(compact_control));
	ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
	ret = papermono_end_write(config, ret);
	ret = ret ?: papermono_wait_ready(config, "compact full-refresh activation");
	ret = ret ?: papermono_deep_sleep(dev, true);
	if (ret < 0) {
		goto out;
	}
#else
	/* First synchronize the controller drive state with the logical LVGL
	 * frame, matching the C153 OTP reference implementation byte-for-byte.
	 */
	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
				 &synchronize_control, sizeof(synchronize_control));
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_1,
					   data->frame, false);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
	ret = papermono_end_write(config, ret);
	ret = ret ?: papermono_wait_ready(config, "full-refresh synchronization");
	if (ret < 0) {
		goto out;
	}

	/* OTP Mode 1 writes the physical final image to both RAM planes, creating
	 * the retained monochrome baseline used by subsequent partial updates.
	 */
	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
				 &otp_control, sizeof(otp_control));
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_2,
					   data->frame, true);
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_1,
					   data->frame, true);
	ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
	ret = papermono_end_write(config, ret);
	ret = ret ?: papermono_wait_ready(config, "full-refresh OTP activation");
	ret = ret ?: papermono_deep_sleep(dev, true);
#endif

	out:
	if (ret == 0) {
		data->baseline_ready = true;
		data->consecutive_partials = 0U;
		data->full_refreshes++;
		data->last_full_refresh_ms = k_uptime_get_32() - started_ms;
		LOG_INF("SSD1677 full refresh %u complete in %u ms; baseline ready",
			data->full_refreshes, data->last_full_refresh_ms);
	} else {
		data->baseline_ready = false;
	}
	return ret;
}

static int papermono_refresh_full_with_recovery(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	int ret;

	ret = papermono_refresh_full(dev);
	if (ret != -ETIMEDOUT && ret != -EIO) {
		return ret;
	}

	/* The C153 panel board has 10 uF storage capacitors on both PREVGH and
	 * PREVGL.  An activation interrupted by a board reset can leave those
	 * rails charged long after EPD_3V3_L3 is switched off.  Keep every panel
	 * input clamped while they discharge before retrying the OTP waveform.
	 */
	LOG_WRN("SSD1677 timed out; performing 15-second deep-discharge recovery");
	ret = papermono_hardware_power_cycle(config, 15000U, 500U);
	return ret ?: papermono_refresh_full(dev);
}

#if defined(CONFIG_PAPERMONO_PARTIAL_REFRESH)
static struct papermono_refresh_policy papermono_refresh_policy(void)
{
	struct papermono_refresh_policy policy = {
		.limit = CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT,
		.opportunistic = IS_ENABLED(
			CONFIG_PAPERMONO_MAINTENANCE_OPPORTUNISTIC),
	};

	return policy;
}

static int papermono_refresh_partial(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	struct papermono_data *data = dev->data;
	static const uint8_t partial_border = 0x80U;
	static const uint8_t display_mode = 0x00U;
	static const uint8_t partial_control = 0xFFU;
	const uint32_t started_ms = k_uptime_get_32();
	int ret;

	if (!data->baseline_ready) {
		return -EAGAIN;
	}

	/* Hardware reset leaves RAM intact and exits deep-sleep mode 1. */
	ret = papermono_hardware_reset(config);
	if (ret == 0) {
		data->controller_sleeping = false;
	}
	ret = ret ?: papermono_wait_ready(config, "partial-refresh reset");
	if (ret < 0) {
		goto out;
	}

	ret = papermono_begin_write(config);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_BORDER_WAVEFORM,
				 &partial_border, sizeof(partial_border));
	ret = ret ?: papermono_write_frame(dev, SSD1677_CMD_WRITE_RAM_1,
					   data->frame, true);
	ret = ret ?: papermono_register(dev, SSD1677_CMD_DISPLAY_MODE,
					&display_mode, sizeof(display_mode));
	ret = ret ?: papermono_register(dev, SSD1677_CMD_UPDATE_CONTROL,
					&partial_control, sizeof(partial_control));
	ret = ret ?: papermono_command(dev, SSD1677_CMD_MASTER_ACTIVATION);
	ret = papermono_end_write(config, ret);
	ret = ret ?: papermono_wait_ready(config, "partial-refresh activation");
	/* Partial control 0xFF already disables the analog supply and clock. */
	ret = ret ?: papermono_deep_sleep(dev, false);

out:
	if (ret == 0) {
		data->consecutive_partials++;
		data->partial_refreshes++;
		data->last_partial_refresh_ms = k_uptime_get_32() - started_ms;
		LOG_INF("SSD1677 partial refresh %u (%u/%u) complete in %u ms",
			data->partial_refreshes, data->consecutive_partials,
			CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT,
			data->last_partial_refresh_ms);
	} else {
		data->baseline_ready = false;
	}

	return ret;
}
#endif

static int papermono_refresh_changed_frame(const struct device *dev)
{
	struct papermono_data *data = dev->data;
	int ret;

	if (!data->dirty) {
		return 0;
	}

#if defined(CONFIG_PAPERMONO_PARTIAL_REFRESH)
	if (data->baseline_ready) {
		const struct papermono_refresh_policy policy =
			papermono_refresh_policy();
		const struct papermono_refresh_policy_state state = {
			.baseline_ready = true,
			.partials = data->consecutive_partials,
			.quality = data->refresh_hint,
		};

		if (papermono_refresh_policy_decide(&policy, &state) ==
		    PAPERMONO_REFRESH_ACTION_FULL) {
			if (data->consecutive_partials >=
			    CONFIG_PAPERMONO_PARTIAL_REFRESH_LIMIT) {
				LOG_INF("Partial-refresh limit reached; "
					"performing maintenance full refresh");
			} else {
				LOG_INF("Performing maintenance full refresh at "
					"a page transition");
			}
			ret = papermono_refresh_full_with_recovery(dev);
		} else {
			ret = papermono_refresh_partial(dev);
			if (ret < 0) {
				data->partial_fallbacks++;
				LOG_WRN("Partial refresh failed (%d); falling back to full refresh", ret);
				ret = papermono_refresh_full_with_recovery(dev);
			}
		}
	} else {
		ret = papermono_refresh_full_with_recovery(dev);
	}
#else
	ret = papermono_refresh_full_with_recovery(dev);
#endif

	if (ret == 0) {
		data->dirty = false;
	}
	return ret;
}

static int papermono_blanking_on(const struct device *dev)
{
	struct papermono_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->blanking = true;
	k_mutex_unlock(&data->lock);
	return 0;
}

static int papermono_blanking_off(const struct device *dev)
{
	struct papermono_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->blanking) {
		data->blanking = false;
		ret = papermono_refresh_changed_frame(dev);
	}
	/* The hint only applies to the render that just completed. */
	data->refresh_hint = PAPERMONO_REFRESH_AUTO;
	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_write(const struct device *dev, uint16_t x, uint16_t y,
			   const struct display_buffer_descriptor *desc,
			   const void *buffer)
{
	struct papermono_data *data = dev->data;
	const uint8_t *source = buffer;
	const size_t expected = (size_t)desc->width * desc->height / 8U;
	int ret = 0;

	if (buffer == NULL || desc->width == 0U || desc->height == 0U ||
	    desc->pitch != desc->width || (y % 8U) != 0U ||
	    (desc->height % 8U) != 0U || desc->buf_size < expected ||
	    x + desc->width > VISIBLE_WIDTH || y + desc->height > VISIBLE_HEIGHT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * Zephyr's MONO_VTILED buffer is arranged as [vertical byte][x].
	 * SSD1677 RAM for this panel is arranged as [visible x][vertical byte].
	 * Transposing bytes preserves their MSB-first top-to-bottom pixel order.
	 */
	for (uint16_t byte_y = 0U; byte_y < desc->height / 8U; ++byte_y) {
		for (uint16_t column = 0U; column < desc->width; ++column) {
			const size_t src_index = (size_t)byte_y * desc->width + column;
			const size_t dst_index = (size_t)(x + column) * RAW_ROW_BYTES +
						 (y / 8U) + byte_y;

			if (data->frame[dst_index] != source[src_index]) {
				data->frame[dst_index] = source[src_index];
				data->dirty = true;
			}
		}
	}

	if (!data->blanking && !desc->frame_incomplete) {
		ret = papermono_refresh_changed_frame(dev);
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_read(const struct device *dev, uint16_t x, uint16_t y,
			  const struct display_buffer_descriptor *desc, void *buffer)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(x);
	ARG_UNUSED(y);
	ARG_UNUSED(desc);
	ARG_UNUSED(buffer);
	return -ENOTSUP;
}

int papermono_ssd1677_force_full_next(const struct device *dev)
{
	struct papermono_data *data;

	if (dev == NULL || dev->data == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	data->baseline_ready = false;
	data->consecutive_partials = 0U;
	k_mutex_unlock(&data->lock);
	return 0;
}

int papermono_ssd1677_set_refresh_hint(
	const struct device *dev, enum papermono_refresh_quality quality)
{
	struct papermono_data *data;

	if (dev == NULL || dev->data == NULL) {
		return -EINVAL;
	}
	if (quality != PAPERMONO_REFRESH_AUTO &&
	    quality != PAPERMONO_REFRESH_FAST &&
	    quality != PAPERMONO_REFRESH_FULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	data->refresh_hint = quality;
	k_mutex_unlock(&data->lock);
	return 0;
}

static void papermono_get_capabilities(const struct device *dev,
				       struct display_capabilities *caps)
{
	ARG_UNUSED(dev);
	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = VISIBLE_WIDTH;
	caps->y_resolution = VISIBLE_HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = PIXEL_FORMAT_MONO10;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
	caps->screen_info = SCREEN_INFO_MONO_VTILED |
			    SCREEN_INFO_MONO_MSB_FIRST |
			    SCREEN_INFO_EPD;
}

static int papermono_set_pixel_format(const struct device *dev,
				      enum display_pixel_format format)
{
	ARG_UNUSED(dev);
	return format == PIXEL_FORMAT_MONO10 ? 0 : -ENOTSUP;
}

static int papermono_set_orientation(const struct device *dev,
				     enum display_orientation orientation)
{
	ARG_UNUSED(dev);
	return orientation == DISPLAY_ORIENTATION_NORMAL ? 0 : -ENOTSUP;
}

static int papermono_set_brightness(const struct device *dev,
				    uint8_t brightness)
{
	const struct papermono_config *config = dev->config;
	uint32_t pulse;

	/* Match the perceptual response used by the factory driver. */
	pulse = (uint64_t)config->frontlight.period * brightness * brightness /
		(UINT8_MAX * UINT8_MAX);
	return pwm_set_pulse_dt(&config->frontlight, pulse);
}

static int papermono_init(const struct device *dev)
{
	const struct papermono_config *config = dev->config;
	struct papermono_data *data = dev->data;
	int ret;

	BUILD_ASSERT(DT_PROP(DISPLAY_NODE, width) == VISIBLE_WIDTH);
	BUILD_ASSERT(DT_PROP(DISPLAY_NODE, height) == VISIBLE_HEIGHT);

	if (!spi_is_ready_dt(&config->spi) ||
	    !gpio_is_ready_dt(&config->cs) || !gpio_is_ready_dt(&config->dc) ||
	    !gpio_is_ready_dt(&config->busy) ||
	    !gpio_is_ready_dt(&config->power) ||
	    !gpio_is_ready_dt(&config->reset) ||
	    !pwm_is_ready_dt(&config->frontlight)) {
		LOG_ERR("PaperMono display bus is not ready");
		return -ENODEV;
	}

	/* Start with the panel rail off and every direct panel input physically low.
	 * papermono_hardware_reset() below performs the ordered power-up and a
	 * complete reset pulse after the rail is stable, then releases RESET through
	 * the M5IOE1 pull-up.
	 */
	ret = gpio_pin_configure_dt(&config->cs, GPIO_OUTPUT_INACTIVE);
	ret = ret ?: gpio_pin_configure_dt(&config->dc, GPIO_OUTPUT_INACTIVE);
	ret = ret ?: gpio_pin_configure_dt(&config->busy,
					  GPIO_INPUT | GPIO_PULL_UP);
	ret = ret ?: papermono_reset_assert(config);
	ret = ret ?: gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
	ret = ret ?: papermono_set_brightness(dev, 0U);
	if (ret < 0) {
		return ret;
	}

	k_mutex_init(&data->lock);
	data->blanking = false;
	data->dirty = false;
	data->baseline_ready = false;
	data->controller_sleeping = false;
	data->consecutive_partials = 0U;
	data->refresh_hint = PAPERMONO_REFRESH_AUTO;
	data->full_refreshes = 0U;
	data->partial_refreshes = 0U;
	data->partial_fallbacks = 0U;
	data->gray_refreshes = 0U;
	data->last_full_refresh_ms = 0U;
	data->last_partial_refresh_ms = 0U;
	/* PIXEL_FORMAT_MONO10 defines zero as white and one as black. */
	memset(data->frame, 0x00, sizeof(data->frame));

	ret = papermono_hardware_reset(config);
	ret = ret ?: papermono_wait_ready(config, "driver initialization");
	if (ret == -ETIMEDOUT) {
		/* The reset line is routed through an optional series footprint on
		 * some C153 revisions.  If BUSY remains high after the hardware pulse,
		 * issue command 0x12 without a pre-check so a responsive controller can
		 * still leave an interrupted state.
		 */
		LOG_WRN("SSD1677 BUSY at probe; forcing software reset");
		ret = papermono_issue_software_reset(
			dev, "forced probe software reset");
	}
	if (ret == -ETIMEDOUT || ret == -EIO) {
		/* A refresh interrupted before this firmware starts can leave BUSY
		 * asserted, so the normal display API would never become ready and
		 * its refresh-level recovery could not run.  Recover at probe time as
		 * well, while all panel inputs are under this driver's control.
		 */
		LOG_WRN("SSD1677 BUSY at probe; performing 15-second deep-discharge recovery");
		ret = papermono_hardware_power_cycle(config, 15000U, 500U);
		ret = ret ?: papermono_issue_software_reset(
			dev, "driver initialization recovery");
	}
	if (ret < 0) {
		return ret;
	}

	LOG_INF("PaperMono SSD1677 transport ready: 480x800, SPI %u Hz",
		config->spi.config.frequency);
	return 0;
}

static DEVICE_API(display, papermono_api) = {
	.blanking_on = papermono_blanking_on,
	.blanking_off = papermono_blanking_off,
	.write = papermono_write,
	.read = papermono_read,
	.get_capabilities = papermono_get_capabilities,
	.set_pixel_format = papermono_set_pixel_format,
	.set_orientation = papermono_set_orientation,
	.set_brightness = papermono_set_brightness,
};

static const struct papermono_config papermono_config = {
	.spi = {
		.bus = DEVICE_DT_GET(DT_BUS(DISPLAY_NODE)),
		.config = {
			.frequency = DT_PROP(DISPLAY_NODE, spi_max_frequency),
			.operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
			.slave = DT_REG_ADDR(DISPLAY_NODE),
		},
	},
	.cs = GPIO_DT_SPEC_GET(DISPLAY_NODE, cs_gpios),
	.dc = GPIO_DT_SPEC_GET(DISPLAY_NODE, dc_gpios),
	.busy = GPIO_DT_SPEC_GET(DISPLAY_NODE, busy_gpios),
	.power = GPIO_DT_SPEC_GET(DISPLAY_NODE, power_gpios),
	.reset = GPIO_DT_SPEC_GET(DISPLAY_NODE, reset_gpios),
	.frontlight = PWM_DT_SPEC_GET_BY_NAME(DISPLAY_NODE, frontlight),
};

static struct papermono_data papermono_data;

DEVICE_DT_DEFINE(DISPLAY_NODE, papermono_init, NULL,
		 &papermono_data, &papermono_config,
		 POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &papermono_api);
