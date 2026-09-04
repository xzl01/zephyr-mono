/*
 * Copyright (c) 2026 M5Stack Technology CO LTD
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT m5stack_m5ioe1

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <papermono/m5ioe1.h>

LOG_MODULE_REGISTER(m5ioe1, CONFIG_GPIO_LOG_LEVEL);

#define M5IOE1_VALID_PINS       BIT_MASK(M5IOE1_GPIO_COUNT)

#define M5IOE1_REG_UID_L        0x00U
#define M5IOE1_REG_GPIO_MODE_L  0x03U
#define M5IOE1_REG_GPIO_OUT_L   0x05U
#define M5IOE1_REG_GPIO_IN_L    0x07U
#define M5IOE1_REG_GPIO_PU_L    0x09U
#define M5IOE1_REG_GPIO_PD_L    0x0BU
#define M5IOE1_REG_GPIO_IE_L    0x0DU
#define M5IOE1_REG_GPIO_IP_L    0x0FU
#define M5IOE1_REG_GPIO_IS_L    0x11U
#define M5IOE1_REG_GPIO_DRV_L   0x13U
#define M5IOE1_REG_ADC_CTRL     0x15U
#define M5IOE1_REG_ADC_DATA_L   0x16U
#define M5IOE1_REG_TEMP_CTRL    0x18U
#define M5IOE1_REG_TEMP_DATA_L  0x19U
#define M5IOE1_REG_PWM1_DUTY_L  0x1BU
#define M5IOE1_REG_I2C_CFG      0x23U
#define M5IOE1_REG_PWM_FREQ_L   0x25U
#define M5IOE1_REG_REF_VOLT_L   0x27U

#define M5IOE1_ADC_START        BIT(6)
#define M5IOE1_ADC_BUSY         BIT(7)
#define M5IOE1_TEMP_START       BIT(6)
#define M5IOE1_TEMP_BUSY        BIT(7)
#define M5IOE1_PWM_POLARITY     BIT(6)
#define M5IOE1_PWM_ENABLE       BIT(7)
#define M5IOE1_I2C_SLEEP_MASK   0x0FU
#define M5IOE1_I2C_WAKE_RISING  BIT(5)

#define M5IOE1_CONVERSION_TIMEOUT K_MSEC(100)
#define M5IOE1_I2C_ACCESS_DELAY_US 500U
#define M5IOE1_I2C_RECOVERY_DELAY_US 1000U
#define M5IOE1_I2C_MAX_ATTEMPTS 3U

static const uint8_t m5ioe1_pwm_pins[M5IOE1_PWM_COUNT] = {
	8U, 7U, 10U, 9U
};

struct m5ioe1_config {
	struct gpio_driver_config common;
	struct i2c_dt_spec bus;
	struct gpio_dt_spec interrupt;
};

struct m5ioe1_data {
	struct gpio_driver_data common;
	struct k_mutex lock;
	uint16_t mode;
	uint16_t output;
	uint16_t pull_up;
	uint16_t pull_down;
	uint16_t interrupt_enable;
	uint16_t interrupt_polarity;
	uint16_t drive;
	uint16_t pwm_frequency;
	uint8_t pwm_enabled;
	sys_slist_t callbacks;
	struct gpio_callback host_callback;
	struct k_work interrupt_work;
	const struct device *self;
};

static int m5ioe1_read_unlocked(const struct device *dev, uint8_t reg,
				void *buffer, size_t length)
{
	const struct m5ioe1_config *config = dev->config;
	int ret = -EIO;

	/* M5IOE1 is a PY32 MCU emulating an I2C register file.  Its firmware needs
	 * about 500 us to latch the register pointer before it can serve the read.
	 * i2c_write_read_dt() emits both phases back-to-back, which works by chance
	 * on a cold boot but is unreliable after the expander wakes from I2C sleep.
	 */
	for (unsigned int attempt = 0U;
	     attempt < M5IOE1_I2C_MAX_ATTEMPTS; ++attempt) {
		ret = i2c_write_dt(&config->bus, &reg, sizeof(reg));
		k_busy_wait(M5IOE1_I2C_ACCESS_DELAY_US);
		if (ret == 0) {
			ret = i2c_read_dt(&config->bus, buffer, length);
		}
		k_busy_wait(M5IOE1_I2C_ACCESS_DELAY_US);
		if (ret == 0) {
			return 0;
		}
		if (ret != -EFAULT && ret != -ETIMEDOUT && ret != -EBUSY) {
			return ret;
		}

		if (attempt == 0U) {
			LOG_WRN("I2C read 0x%02x failed (%d); recovering bus",
				reg, ret);
		}
		(void)i2c_recover_bus(config->bus.bus);
		k_busy_wait(M5IOE1_I2C_RECOVERY_DELAY_US);
	}

	LOG_ERR("I2C read 0x%02x still failing after %u attempts (%d)",
		reg, M5IOE1_I2C_MAX_ATTEMPTS, ret);
	return ret;
}

static int m5ioe1_write_unlocked(const struct device *dev, uint8_t reg,
				 const void *buffer, size_t length)
{
	const struct m5ioe1_config *config = dev->config;
	uint8_t bytes[65];
	int ret = -EIO;

	if (length > sizeof(bytes) - 1U) {
		return -EMSGSIZE;
	}

	bytes[0] = reg;
	memcpy(&bytes[1], buffer, length);
	for (unsigned int attempt = 0U;
	     attempt < M5IOE1_I2C_MAX_ATTEMPTS; ++attempt) {
		ret = i2c_write_dt(&config->bus, bytes, length + 1U);
		k_busy_wait(M5IOE1_I2C_ACCESS_DELAY_US);
		if (ret == 0) {
			return 0;
		}
		if (ret != -EFAULT && ret != -ETIMEDOUT && ret != -EBUSY) {
			return ret;
		}

		if (attempt == 0U) {
			LOG_WRN("I2C write 0x%02x failed (%d); recovering bus",
				reg, ret);
		}
		(void)i2c_recover_bus(config->bus.bus);
		k_busy_wait(M5IOE1_I2C_RECOVERY_DELAY_US);
	}

	LOG_ERR("I2C write 0x%02x still failing after %u attempts (%d)",
		reg, M5IOE1_I2C_MAX_ATTEMPTS, ret);
	return ret;
}

static int m5ioe1_read16_unlocked(const struct device *dev, uint8_t reg,
				  uint16_t *value)
{
	uint8_t bytes[2];
	int ret;

	ret = m5ioe1_read_unlocked(dev, reg, bytes, sizeof(bytes));
	if (ret == 0) {
		*value = sys_get_le16(bytes);
	}
	return ret;
}

static int m5ioe1_write16_unlocked(const struct device *dev, uint8_t reg,
				   uint16_t value)
{
	uint8_t bytes[2];

	sys_put_le16(value, bytes);
	return m5ioe1_write_unlocked(dev, reg, bytes, sizeof(bytes));
}

static int m5ioe1_release_pwm_unlocked(const struct device *dev,
				       gpio_pin_t pin)
{
	struct m5ioe1_data *data = dev->data;
	const uint16_t enable_mask = (uint16_t)M5IOE1_PWM_ENABLE << 8;
	uint16_t pwm;
	int ret;

	for (uint8_t channel = 0U; channel < ARRAY_SIZE(m5ioe1_pwm_pins);
	     channel++) {
		if (m5ioe1_pwm_pins[channel] != pin ||
		    (data->pwm_enabled & BIT(channel)) == 0U) {
			continue;
		}

		ret = m5ioe1_read16_unlocked(
			dev, M5IOE1_REG_PWM1_DUTY_L + channel * 2U, &pwm);
		if (ret < 0) {
			return ret;
		}
		ret = m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_PWM1_DUTY_L + channel * 2U,
			pwm & ~enable_mask);
		if (ret < 0) {
			return ret;
		}
		data->pwm_enabled &= ~BIT(channel);
		break;
	}

	return 0;
}

int m5ioe1_reg_read(const struct device *dev, uint8_t reg, void *buffer,
		    size_t length)
{
	struct m5ioe1_data *data;
	int ret;

	if (dev == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, reg, buffer, length);
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5ioe1_reg_write(const struct device *dev, uint8_t reg,
		     const void *buffer, size_t length)
{
	struct m5ioe1_data *data;
	int ret;

	if (dev == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_write_unlocked(dev, reg, buffer, length);
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5ioe1_get_info(const struct device *dev, struct m5ioe1_info *info)
{
	struct m5ioe1_data *data;
	uint8_t identity[3];
	uint8_t reference[2];
	int ret;

	if (dev == NULL || info == NULL) {
		return -EINVAL;
	}

	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_UID_L, identity,
				   sizeof(identity));
	ret = ret ?: m5ioe1_read_unlocked(dev, M5IOE1_REG_REF_VOLT_L,
					  reference, sizeof(reference));
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		info->uid = sys_get_le16(identity);
		info->revision = identity[2];
		info->reference_mv = sys_get_le16(reference);
	}
	return ret;
}

static int m5ioe1_pin_configure(const struct device *dev, gpio_pin_t pin,
				gpio_flags_t flags)
{
	struct m5ioe1_data *data = dev->data;
	uint16_t mode;
	uint16_t output;
	uint16_t pull_up;
	uint16_t pull_down;
	uint16_t drive;
	const uint16_t bit = BIT(pin);
	bool output_mode;
	int ret = 0;

	if (pin >= M5IOE1_GPIO_COUNT) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if ((flags & (GPIO_INPUT | GPIO_OUTPUT)) == GPIO_DISCONNECTED ||
	    (flags & (GPIO_INPUT | GPIO_OUTPUT)) == (GPIO_INPUT | GPIO_OUTPUT) ||
	    (flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) ==
					 (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
		return -ENOTSUP;
	}
	if ((flags & GPIO_SINGLE_ENDED) != 0U &&
	    (flags & GPIO_OPEN_DRAIN) != GPIO_OPEN_DRAIN) {
		return -ENOTSUP;
	}

	output_mode = (flags & GPIO_OUTPUT) != 0U;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_release_pwm_unlocked(dev, pin);
	if (ret < 0) {
		goto out;
	}
	mode = data->mode;
	output = data->output;
	pull_up = data->pull_up;
	pull_down = data->pull_down;
	drive = data->drive;

	if (output_mode && (flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
		output |= bit;
	} else if (output_mode && (flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
		output &= ~bit;
	}
	if (output_mode &&
	    (flags & (GPIO_OUTPUT_INIT_HIGH | GPIO_OUTPUT_INIT_LOW)) != 0U) {
		ret = m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_OUT_L,
					      output);
	}

	if ((flags & GPIO_PULL_UP) != 0U) {
		pull_up |= bit;
		pull_down &= ~bit;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pull_up &= ~bit;
		pull_down |= bit;
	} else {
		pull_up &= ~bit;
		pull_down &= ~bit;
	}
	WRITE_BIT(drive, pin, output_mode &&
		  ((flags & GPIO_OPEN_DRAIN) == GPIO_OPEN_DRAIN));
	WRITE_BIT(mode, pin, output_mode);

	ret = ret ?: m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_PU_L,
					    pull_up);
	ret = ret ?: m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_PD_L,
					    pull_down);
	ret = ret ?: m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_DRV_L,
					    drive);
	ret = ret ?: m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_MODE_L,
					    mode);
	if (ret == 0) {
		data->mode = mode;
		data->output = output;
		data->pull_up = pull_up;
		data->pull_down = pull_down;
		data->drive = drive;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5ioe1_port_get_raw(const struct device *dev,
			       gpio_port_value_t *value)
{
	struct m5ioe1_data *data = dev->data;
	uint16_t input;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read16_unlocked(dev, M5IOE1_REG_GPIO_IN_L, &input);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*value = input & M5IOE1_VALID_PINS;
	}
	return ret;
}

static int m5ioe1_port_set_masked_raw(const struct device *dev,
				      gpio_port_pins_t mask,
				      gpio_port_value_t value)
{
	struct m5ioe1_data *data = dev->data;
	uint16_t output;
	uint16_t actual;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	mask &= M5IOE1_VALID_PINS;
	k_mutex_lock(&data->lock, K_FOREVER);

	/* M5IOE1 is always-on while several Zephyr devices share its output
	 * register.  Re-synchronize from the hardware before every read/modify/
	 * write, then verify the requested pins exactly as the vendor driver does.
	 * This prevents a stale software latch from silently undoing another pin
	 * and catches interrupted I2C writes which otherwise report success.
	 */
	ret = m5ioe1_read16_unlocked(dev, M5IOE1_REG_GPIO_OUT_L, &output);
	if (ret == 0) {
		output = (output & ~mask) | (value & mask);
		ret = m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_OUT_L, output);
	}
	if (ret == 0) {
		ret = m5ioe1_read16_unlocked(
			dev, M5IOE1_REG_GPIO_OUT_L, &actual);
	}
	if (ret == 0 && ((actual ^ output) & mask) != 0U) {
		LOG_ERR("GPIO_OUT verification failed: requested=0x%04x actual=0x%04x mask=0x%04x",
			output, actual, (uint16_t)mask);
		ret = -EIO;
	}
	if (ret == 0) {
		data->output = actual;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5ioe1_port_set_bits_raw(const struct device *dev,
				    gpio_port_pins_t pins)
{
	return m5ioe1_port_set_masked_raw(dev, pins, pins);
}

static int m5ioe1_port_clear_bits_raw(const struct device *dev,
				      gpio_port_pins_t pins)
{
	return m5ioe1_port_set_masked_raw(dev, pins, 0U);
}

static int m5ioe1_port_toggle_bits(const struct device *dev,
				   gpio_port_pins_t pins)
{
	struct m5ioe1_data *data = dev->data;
	uint16_t output;
	uint16_t actual;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	pins &= M5IOE1_VALID_PINS;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read16_unlocked(dev, M5IOE1_REG_GPIO_OUT_L, &output);
	if (ret == 0) {
		output ^= pins;
		ret = m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_OUT_L, output);
	}
	if (ret == 0) {
		ret = m5ioe1_read16_unlocked(
			dev, M5IOE1_REG_GPIO_OUT_L, &actual);
	}
	if (ret == 0 && ((actual ^ output) & pins) != 0U) {
		LOG_ERR("GPIO_OUT toggle verification failed: requested=0x%04x actual=0x%04x mask=0x%04x",
			output, actual, (uint16_t)pins);
		ret = -EIO;
	}
	if (ret == 0) {
		data->output = actual;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5ioe1_pin_interrupt_configure(const struct device *dev,
					  gpio_pin_t pin,
					  enum gpio_int_mode mode,
					  enum gpio_int_trig trig)
{
	struct m5ioe1_data *data = dev->data;
	uint16_t interrupt_enable;
	uint16_t interrupt_polarity;
	uint16_t gpio_mode;
	const uint16_t bit = BIT(pin);
	int ret = 0;

	if (pin >= M5IOE1_GPIO_COUNT) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (mode != GPIO_INT_MODE_DISABLED && mode != GPIO_INT_MODE_EDGE) {
		return -ENOTSUP;
	}
	if (mode != GPIO_INT_MODE_DISABLED &&
	    trig != GPIO_INT_TRIG_LOW && trig != GPIO_INT_TRIG_HIGH) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	interrupt_enable = data->interrupt_enable;
	interrupt_polarity = data->interrupt_polarity;
	gpio_mode = data->mode;
	if (mode == GPIO_INT_MODE_DISABLED) {
		interrupt_enable &= ~bit;
	} else {
		gpio_mode &= ~bit;
		WRITE_BIT(interrupt_polarity, pin, trig == GPIO_INT_TRIG_HIGH);
		interrupt_enable |= bit;
		ret = m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_MODE_L,
					      gpio_mode);
		ret = ret ?: m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_IP_L, interrupt_polarity);
	}
	ret = ret ?: m5ioe1_write16_unlocked(dev, M5IOE1_REG_GPIO_IS_L, 0U);
	ret = ret ?: m5ioe1_write16_unlocked(
		dev, M5IOE1_REG_GPIO_IE_L, interrupt_enable);
	if (ret == 0) {
		data->mode = gpio_mode;
		data->interrupt_polarity = interrupt_polarity;
		data->interrupt_enable = interrupt_enable;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5ioe1_manage_callback(const struct device *dev,
				  struct gpio_callback *callback, bool set)
{
	struct m5ioe1_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void m5ioe1_interrupt_work_handler(struct k_work *work)
{
	struct m5ioe1_data *data = CONTAINER_OF(
		work, struct m5ioe1_data, interrupt_work);
	uint16_t status = 0U;
	int ret;

	/* The interrupt status register is cleared by writing the bits to keep,
	 * so writing 0U acknowledges everything. Clearing unconditionally would
	 * also discard any edge latched between the read and the write, and a
	 * second k_work_submit() is a no-op while this handler runs, so that
	 * edge would never be serviced. Only the bits actually handled below
	 * are acknowledged; the rest stay pending and retrigger the work item.
	 */
	uint16_t handled;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read16_unlocked(data->self, M5IOE1_REG_GPIO_IS_L,
				      &status);
	handled = status & M5IOE1_VALID_PINS;
	if (ret == 0 && handled != 0U) {
		ret = m5ioe1_write16_unlocked(data->self,
					     M5IOE1_REG_GPIO_IS_L, ~handled);
	}
	k_mutex_unlock(&data->lock);

	if (ret == 0 && handled != 0U) {
		gpio_fire_callbacks(&data->callbacks, data->self, handled);
	} else if (ret < 0) {
		LOG_ERR("Failed to service M5IOE1 interrupt (%d)", ret);
	}

	/* An edge latched after the read above is still pending: the host
	 * interrupt line may already have been deasserted, so re-run rather
	 * than wait for the next one. Bounded by handled != 0 above, this
	 * cannot spin on a permanently asserted line.
	 */
	if (ret == 0 && handled != 0U) {
		int submit_ret = k_work_submit(&data->interrupt_work);

		if (submit_ret < 0) {
			LOG_ERR("Failed to reschedule M5IOE1 work (%d)",
				submit_ret);
		}
	}
}

static void m5ioe1_host_interrupt(const struct device *port,
				  struct gpio_callback *callback,
				  gpio_port_pins_t pins)
{
	struct m5ioe1_data *data = CONTAINER_OF(
		callback, struct m5ioe1_data, host_callback);
	int ret;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	ret = k_work_submit(&data->interrupt_work);
	if (ret < 0) {
		LOG_ERR("Failed to schedule M5IOE1 interrupt work (%d)", ret);
	}
}

static int m5ioe1_wait_conversion_unlocked(const struct device *dev,
					   uint8_t control_reg,
					   uint8_t busy_mask)
{
	const int64_t deadline = k_uptime_get() +
		k_ticks_to_ms_floor64(M5IOE1_CONVERSION_TIMEOUT.ticks);
	uint8_t control;
	int ret;

	do {
		ret = m5ioe1_read_unlocked(dev, control_reg, &control,
					    sizeof(control));
		if (ret < 0 || (control & busy_mask) == 0U) {
			return ret;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

int m5ioe1_read_adc(const struct device *dev, uint8_t channel,
		    uint16_t *sample)
{
	struct m5ioe1_data *data;
	uint8_t control;
	uint16_t result;
	int ret;

	if (dev == NULL || sample == NULL || channel < 1U || channel > 4U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	control = M5IOE1_ADC_START | channel;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_write_unlocked(dev, M5IOE1_REG_ADC_CTRL, &control,
				     sizeof(control));
	ret = ret ?: m5ioe1_wait_conversion_unlocked(
		dev, M5IOE1_REG_ADC_CTRL, M5IOE1_ADC_BUSY);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_ADC_DATA_L, &result);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*sample = result & 0x0FFFU;
	}
	return ret;
}

int m5ioe1_read_temperature(const struct device *dev, uint16_t *sample)
{
	struct m5ioe1_data *data;
	uint8_t control = M5IOE1_TEMP_START;
	uint16_t result;
	int ret;

	if (dev == NULL || sample == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_write_unlocked(dev, M5IOE1_REG_TEMP_CTRL, &control,
				     sizeof(control));
	ret = ret ?: m5ioe1_wait_conversion_unlocked(
		dev, M5IOE1_REG_TEMP_CTRL, M5IOE1_TEMP_BUSY);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_TEMP_DATA_L, &result);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*sample = result & 0x0FFFU;
	}
	return ret;
}

int m5ioe1_pwm_set(const struct device *dev, uint8_t channel,
		   uint16_t frequency_hz, uint16_t duty_12bit,
		   bool inverted, bool enable)
{
	struct m5ioe1_data *data;
	const uint8_t channel_bit = BIT(channel);
	uint8_t duty[2];
	uint8_t other_enabled;
	uint16_t mode;
	uint16_t pull_up;
	uint16_t pull_down;
	uint16_t drive;
	uint8_t pin;
	int ret = 0;

	if (dev == NULL || channel >= M5IOE1_PWM_COUNT ||
	    duty_12bit > 0x0FFFU || (enable && frequency_hz == 0U)) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	pin = m5ioe1_pwm_pins[channel];
	k_mutex_lock(&data->lock, K_FOREVER);
	other_enabled = data->pwm_enabled & ~channel_bit;
	if (enable && other_enabled != 0U &&
	    data->pwm_frequency != frequency_hz) {
		ret = -EBUSY;
		goto out;
	}

	if (enable && data->pwm_frequency != frequency_hz) {
		ret = m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_PWM_FREQ_L, frequency_hz);
		if (ret < 0) {
			goto out;
		}
		data->pwm_frequency = frequency_hz;
	}

	duty[0] = (uint8_t)duty_12bit;
	duty[1] = (uint8_t)(duty_12bit >> 8);
	if (inverted) {
		duty[1] |= M5IOE1_PWM_POLARITY;
	}
	if (enable) {
		duty[1] |= M5IOE1_PWM_ENABLE;
	}
	ret = m5ioe1_write_unlocked(
		dev, M5IOE1_REG_PWM1_DUTY_L + channel * 2U,
		duty, sizeof(duty));
	if (ret < 0) {
		goto out;
	}
	WRITE_BIT(data->pwm_enabled, channel, enable);

	if (enable) {
		mode = data->mode | BIT(pin);
		drive = data->drive & ~BIT(pin);
		pull_up = data->pull_up & ~BIT(pin);
		pull_down = data->pull_down & ~BIT(pin);
		ret = m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_PU_L, pull_up);
		ret = ret ?: m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_PD_L, pull_down);
		ret = ret ?: m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_DRV_L, drive);
		ret = ret ?: m5ioe1_write16_unlocked(
			dev, M5IOE1_REG_GPIO_MODE_L, mode);
		if (ret == 0) {
			data->mode = mode;
			data->drive = drive;
			data->pull_up = pull_up;
			data->pull_down = pull_down;
		}
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5ioe1_set_i2c_sleep_timeout(const struct device *dev, uint8_t seconds)
{
	struct m5ioe1_data *data;
	uint8_t config;
	uint8_t actual;
	int ret;

	if (dev == NULL || seconds > M5IOE1_I2C_SLEEP_MASK) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
				   &config, sizeof(config));
	if (ret == 0) {
		config = (config & ~M5IOE1_I2C_SLEEP_MASK) |
			 (seconds & M5IOE1_I2C_SLEEP_MASK);
		ret = m5ioe1_write_unlocked(dev, M5IOE1_REG_I2C_CFG,
					    &config, sizeof(config));
	}
	if (ret == 0) {
		ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
					   &actual, sizeof(actual));
	}
	k_mutex_unlock(&data->lock);
	if (ret == 0 && (actual & M5IOE1_I2C_SLEEP_MASK) != seconds) {
		return -EIO;
	}
	return ret;
}

int m5ioe1_get_i2c_sleep_timeout(const struct device *dev, uint8_t *seconds)
{
	struct m5ioe1_data *data;
	int ret;

	if (dev == NULL || seconds == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
				   seconds, sizeof(*seconds));
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*seconds &= M5IOE1_I2C_SLEEP_MASK;
	}
	return ret;
}

int m5ioe1_set_i2c_wake_edge(const struct device *dev,
			      enum m5ioe1_i2c_wake_edge edge)
{
	struct m5ioe1_data *data;
	uint8_t config;
	uint8_t actual;
	int ret;

	if (dev == NULL || edge > M5IOE1_I2C_WAKE_RISING) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
				   &config, sizeof(config));
	if (ret == 0) {
		WRITE_BIT(config, 5, edge == M5IOE1_I2C_WAKE_RISING);
		ret = m5ioe1_write_unlocked(dev, M5IOE1_REG_I2C_CFG,
					    &config, sizeof(config));
	}
	if (ret == 0) {
		ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
					   &actual, sizeof(actual));
	}
	k_mutex_unlock(&data->lock);
	if (ret == 0 && !!(actual & M5IOE1_I2C_WAKE_RISING) !=
			!!(config & M5IOE1_I2C_WAKE_RISING)) {
		return -EIO;
	}
	return ret;
}

int m5ioe1_get_i2c_wake_edge(const struct device *dev,
			      enum m5ioe1_i2c_wake_edge *edge)
{
	struct m5ioe1_data *data;
	uint8_t config;
	int ret;

	if (dev == NULL || edge == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read_unlocked(dev, M5IOE1_REG_I2C_CFG,
				   &config, sizeof(config));
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*edge = (config & M5IOE1_I2C_WAKE_RISING) != 0U ?
			M5IOE1_I2C_WAKE_RISING : M5IOE1_I2C_WAKE_FALLING;
	}
	return ret;
}

int m5ioe1_send_wake_signal(const struct device *dev)
{
	const struct m5ioe1_config *config;
	struct m5ioe1_data *data;
	uint8_t revision;
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	config = dev->config;
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = i2c_write(config->bus.bus, NULL, 0U, config->bus.addr);
	if (ret == 0) {
		k_busy_wait(500);
		ret = m5ioe1_read_unlocked(dev, 0x02U,
					   &revision, sizeof(revision));
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_API(gpio, m5ioe1_gpio_api) = {
	.pin_configure = m5ioe1_pin_configure,
	.port_get_raw = m5ioe1_port_get_raw,
	.port_set_masked_raw = m5ioe1_port_set_masked_raw,
	.port_set_bits_raw = m5ioe1_port_set_bits_raw,
	.port_clear_bits_raw = m5ioe1_port_clear_bits_raw,
	.port_toggle_bits = m5ioe1_port_toggle_bits,
	.pin_interrupt_configure = m5ioe1_pin_interrupt_configure,
	.manage_callback = m5ioe1_manage_callback,
};

static int m5ioe1_init(const struct device *dev)
{
	const struct m5ioe1_config *config = dev->config;
	struct m5ioe1_data *data = dev->data;
	struct m5ioe1_info info;
	uint8_t i2c_config = 0U;
	uint16_t pwm;
	unsigned int attempt;
	int ret;

	if (!i2c_is_ready_dt(&config->bus)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	sys_slist_init(&data->callbacks);
	k_work_init(&data->interrupt_work, m5ioe1_interrupt_work_handler);
	data->self = dev;

	/* Match the official UserDemo startup guard: M5IOE1 and the shared
	 * board I2C rail can need the full interval after a PMIC cold start.
	 */
	k_msleep(500);
	for (attempt = 0U; attempt < 8U; ++attempt) {
		ret = m5ioe1_get_info(dev, &info);
		if (ret == 0) {
			break;
		}

		/* A debugger reset can interrupt an in-flight transfer while the
		 * external controller remains powered.  Reset the ESP32 I2C FSM and
		 * clock out any held SDA state before probing again.
		 */
		(void)i2c_recover_bus(config->bus.bus);
		k_msleep(50);
	}
	if (ret < 0) {
		LOG_ERR("M5IOE1 at 0x%02x is not responding after %u attempts (%d)",
			config->bus.addr, attempt, ret);
		return ret;
	}

	ret = m5ioe1_reg_write(dev, M5IOE1_REG_I2C_CFG, &i2c_config,
				sizeof(i2c_config));
	if (ret < 0) {
		return ret;
	}
	k_msleep(2);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_MODE_L, &data->mode);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_OUT_L, &data->output);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_PU_L, &data->pull_up);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_PD_L, &data->pull_down);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_IE_L, &data->interrupt_enable);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_IP_L, &data->interrupt_polarity);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_GPIO_DRV_L, &data->drive);
	ret = ret ?: m5ioe1_read16_unlocked(
		dev, M5IOE1_REG_PWM_FREQ_L, &data->pwm_frequency);
	for (uint8_t channel = 0U;
	     ret == 0 && channel < M5IOE1_PWM_COUNT; ++channel) {
		ret = m5ioe1_read16_unlocked(
			dev, M5IOE1_REG_PWM1_DUTY_L + channel * 2U, &pwm);
		if (ret == 0 &&
		    (pwm & ((uint16_t)M5IOE1_PWM_ENABLE << 8)) != 0U) {
			data->pwm_enabled |= BIT(channel);
		}
	}
	k_mutex_unlock(&data->lock);
	if (ret < 0) {
		return ret;
	}

	if (config->interrupt.port != NULL) {
		if (!gpio_is_ready_dt(&config->interrupt)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(
			&config->interrupt, GPIO_INPUT | GPIO_PULL_UP);
		if (ret < 0) {
			return ret;
		}
		gpio_init_callback(&data->host_callback, m5ioe1_host_interrupt,
				   BIT(config->interrupt.pin));
		ret = gpio_add_callback(
			config->interrupt.port, &data->host_callback);
		ret = ret ?: gpio_pin_interrupt_configure_dt(
			&config->interrupt, GPIO_INT_EDGE_TO_ACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	LOG_INF("M5IOE1 uid=0x%04x revision=0x%02x vref=%u mV ready",
		info.uid, info.revision, info.reference_mv);
	return 0;
}

#define M5IOE1_DEFINE(inst)                                                   \
	static const struct m5ioe1_config m5ioe1_config_##inst = {             \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(inst),                  \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                \
		.interrupt = GPIO_DT_SPEC_INST_GET_OR(inst, interrupt_gpios, {}), \
	};                                                                         \
	static struct m5ioe1_data m5ioe1_data_##inst;                              \
	DEVICE_DT_INST_DEFINE(inst, m5ioe1_init, NULL, &m5ioe1_data_##inst,         \
			      &m5ioe1_config_##inst, POST_KERNEL,                 \
			      CONFIG_PAPERMONO_MFD_INIT_PRIORITY,                  \
			      &m5ioe1_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(M5IOE1_DEFINE)
