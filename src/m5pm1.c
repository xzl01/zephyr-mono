/*
 * Copyright (c) 2026 M5Stack Technology CO LTD
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT m5stack_m5pm1

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

#include <papermono/m5pm1.h>

LOG_MODULE_REGISTER(m5pm1, CONFIG_GPIO_LOG_LEVEL);

#define M5PM1_REG_DEVICE_ID      0x00U
#define M5PM1_REG_POWER_SOURCE   0x04U
#define M5PM1_REG_WAKE_SOURCE    0x05U
#define M5PM1_REG_POWER_CONFIG   0x06U
#define M5PM1_REG_HOLD_CONFIG    0x07U
#define M5PM1_REG_BATTERY_LVP    0x08U
#define M5PM1_REG_I2C_CONFIG     0x09U
#define M5PM1_REG_WATCHDOG_COUNT 0x0AU
#define M5PM1_REG_WATCHDOG_KEY   0x0BU
#define M5PM1_REG_SYSTEM_COMMAND 0x0CU
#define M5PM1_REG_GPIO_MODE      0x10U
#define M5PM1_REG_GPIO_OUTPUT    0x11U
#define M5PM1_REG_GPIO_INPUT     0x12U
#define M5PM1_REG_GPIO_DRIVE     0x13U
#define M5PM1_REG_GPIO_PULL0     0x14U
#define M5PM1_REG_GPIO_PULL1     0x15U
#define M5PM1_REG_GPIO_FUNC0     0x16U
#define M5PM1_REG_GPIO_FUNC1     0x17U
#define M5PM1_REG_GPIO_WAKE_EN   0x18U
#define M5PM1_REG_GPIO_WAKE_CFG  0x19U
#define M5PM1_REG_VREF_L         0x20U
#define M5PM1_REG_VBAT_L         0x22U
#define M5PM1_REG_VIN_L          0x24U
#define M5PM1_REG_5VOUT_L        0x26U
#define M5PM1_REG_ADC_RESULT_L   0x28U
#define M5PM1_REG_ADC_CONTROL    0x2AU
#define M5PM1_REG_PWM0_L         0x30U
#define M5PM1_REG_PWM_FREQUENCY  0x34U
#define M5PM1_REG_IRQ_STATUS1    0x40U
#define M5PM1_REG_IRQ_STATUS2    0x41U
#define M5PM1_REG_IRQ_STATUS3    0x42U
#define M5PM1_REG_IRQ_MASK1      0x43U
#define M5PM1_REG_RTC_RAM        0xA0U

#define M5PM1_FUNC_GPIO          0U
#define M5PM1_FUNC_IRQ           1U
#define M5PM1_FUNC_WAKE          2U
#define M5PM1_FUNC_OTHER         3U
#define M5PM1_PWM_ENABLE         BIT(4)
#define M5PM1_PWM_POLARITY       BIT(5)
#define M5PM1_LED_DRIVE_OD       BIT(5)
#define M5PM1_WATCHDOG_FEED_KEY  0xA5U
#define M5PM1_SYSTEM_KEY         0xA0U
#define M5PM1_VALID_PINS         BIT_MASK(M5PM1_GPIO_COUNT)
#define M5PM1_I2C_ACCESS_DELAY_US 500U
#define M5PM1_I2C_RECOVERY_DELAY_US 1000U
#define M5PM1_I2C_MAX_ATTEMPTS   3U

static const uint8_t m5pm1_pwm_pins[M5PM1_PWM_COUNT] = {3U, 4U};

struct m5pm1_config {
	struct gpio_driver_config common;
	struct i2c_dt_spec bus;
	struct gpio_dt_spec interrupt;
	uint8_t irq_output_pin;
};

struct m5pm1_data {
	struct gpio_driver_data common;
	struct k_mutex lock;
	uint8_t mode;
	uint8_t output;
	uint8_t drive;
	uint8_t pull0;
	uint8_t pull1;
	uint8_t func0;
	uint8_t func1;
	uint8_t irq_mask1;
	uint8_t edge_rising;
	uint8_t edge_falling;
	uint8_t last_input;
	uint8_t boot_wake_source;
	uint8_t boot_wake_gpio_mask;
	uint16_t pwm_frequency;
	uint8_t pwm_enabled;
	struct m5pm1_irq_status pending_irq;
	sys_slist_t callbacks;
	struct gpio_callback host_callback;
	struct k_work interrupt_work;
	const struct device *self;
};

static int m5pm1_read_unlocked(const struct device *dev, uint8_t reg,
			       void *buffer, size_t length)
{
	const struct m5pm1_config *config = dev->config;
	int ret = -EIO;

	/* M5PM1 is a firmware-emulated I2C slave.  Keep the official 500 us
	 * interval between the register-pointer write and the data read instead of
	 * issuing a conventional back-to-back combined transaction.
	 */
	for (unsigned int attempt = 0U;
	     attempt < M5PM1_I2C_MAX_ATTEMPTS; ++attempt) {
		ret = i2c_write_dt(&config->bus, &reg, sizeof(reg));
		k_busy_wait(M5PM1_I2C_ACCESS_DELAY_US);
		if (ret == 0) {
			ret = i2c_read_dt(&config->bus, buffer, length);
		}
		k_busy_wait(M5PM1_I2C_ACCESS_DELAY_US);
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
		k_busy_wait(M5PM1_I2C_RECOVERY_DELAY_US);
	}

	LOG_ERR("I2C read 0x%02x still failing after %u attempts (%d)",
		reg, M5PM1_I2C_MAX_ATTEMPTS, ret);
	return ret;
}

static int m5pm1_write_unlocked(const struct device *dev, uint8_t reg,
				const void *buffer, size_t length)
{
	const struct m5pm1_config *config = dev->config;
	uint8_t bytes[65];
	int ret = -EIO;

	if (length > sizeof(bytes) - 1U) {
		return -EMSGSIZE;
	}
	bytes[0] = reg;
	memcpy(&bytes[1], buffer, length);
	for (unsigned int attempt = 0U;
	     attempt < M5PM1_I2C_MAX_ATTEMPTS; ++attempt) {
		ret = i2c_write_dt(&config->bus, bytes, length + 1U);
		k_busy_wait(M5PM1_I2C_ACCESS_DELAY_US);
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
		k_busy_wait(M5PM1_I2C_RECOVERY_DELAY_US);
	}

	LOG_ERR("I2C write 0x%02x still failing after %u attempts (%d)",
		reg, M5PM1_I2C_MAX_ATTEMPTS, ret);
	return ret;
}

static int m5pm1_read8_unlocked(const struct device *dev, uint8_t reg,
				uint8_t *value)
{
	return m5pm1_read_unlocked(dev, reg, value, sizeof(*value));
}

static int m5pm1_write8_unlocked(const struct device *dev, uint8_t reg,
				 uint8_t value)
{
	return m5pm1_write_unlocked(dev, reg, &value, sizeof(value));
}

static int m5pm1_read16_unlocked(const struct device *dev, uint8_t reg,
				 uint16_t *value)
{
	uint8_t bytes[2];
	int ret;

	ret = m5pm1_read_unlocked(dev, reg, bytes, sizeof(bytes));
	if (ret == 0) {
		*value = sys_get_le16(bytes);
	}
	return ret;
}

static int m5pm1_write16_unlocked(const struct device *dev, uint8_t reg,
				  uint16_t value)
{
	uint8_t bytes[2];

	sys_put_le16(value, bytes);
	return m5pm1_write_unlocked(dev, reg, bytes, sizeof(bytes));
}

static int m5pm1_release_pwm_unlocked(const struct device *dev,
				      gpio_pin_t pin)
{
	struct m5pm1_data *data = dev->data;
	const uint16_t enable_mask = (uint16_t)M5PM1_PWM_ENABLE << 8;
	uint16_t pwm;
	int ret;

	for (uint8_t channel = 0U; channel < ARRAY_SIZE(m5pm1_pwm_pins);
	     channel++) {
		if (m5pm1_pwm_pins[channel] != pin ||
		    (data->pwm_enabled & BIT(channel)) == 0U) {
			continue;
		}

		ret = m5pm1_read16_unlocked(
			dev, M5PM1_REG_PWM0_L + channel * 2U, &pwm);
		if (ret < 0) {
			return ret;
		}
		ret = m5pm1_write16_unlocked(
			dev, M5PM1_REG_PWM0_L + channel * 2U,
			pwm & ~enable_mask);
		if (ret < 0) {
			return ret;
		}
		data->pwm_enabled &= ~BIT(channel);
		break;
	}

	return 0;
}

static void m5pm1_set_2bit_field(uint8_t *low, uint8_t *high,
				 uint8_t pin, uint8_t value)
{
	uint8_t *reg = pin < 4U ? low : high;
	const uint8_t shift = pin < 4U ? pin * 2U : 0U;

	*reg = (*reg & ~(0x03U << shift)) | ((value & 0x03U) << shift);
}

int m5pm1_reg_read(const struct device *dev, uint8_t reg, void *buffer,
		   size_t length)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read_unlocked(dev, reg, buffer, length);
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_reg_write(const struct device *dev, uint8_t reg,
		    const void *buffer, size_t length)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_write_unlocked(dev, reg, buffer, length);
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_get_info(const struct device *dev, struct m5pm1_info *info)
{
	struct m5pm1_data *data;
	uint8_t identity[4];
	int ret;

	if (dev == NULL || info == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read_unlocked(dev, M5PM1_REG_DEVICE_ID, identity,
				  sizeof(identity));
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		info->id = identity[0];
		info->model = identity[1];
		info->hardware_revision = identity[2];
		info->software_revision = identity[3];
	}
	return ret;
}

static int m5pm1_pin_configure(const struct device *dev, gpio_pin_t pin,
			       gpio_flags_t flags)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	uint8_t mode;
	uint8_t output;
	uint8_t drive;
	uint8_t pull0;
	uint8_t pull1;
	uint8_t func0;
	uint8_t func1;
	const uint8_t bit = BIT(pin);
	const bool output_mode = (flags & GPIO_OUTPUT) != 0U;
	uint8_t pull;
	int ret = 0;

	if (pin >= M5PM1_GPIO_COUNT ||
	    (BIT(pin) & config->common.port_pin_mask) == 0U) {
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

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_release_pwm_unlocked(dev, pin);
	if (ret < 0) {
		goto out;
	}
	mode = data->mode;
	output = data->output;
	drive = data->drive;
	pull0 = data->pull0;
	pull1 = data->pull1;
	func0 = data->func0;
	func1 = data->func1;

	if (output_mode && (flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
		output |= bit;
	} else if (output_mode && (flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
		output &= ~bit;
	}
	if (output_mode &&
	    (flags & (GPIO_OUTPUT_INIT_HIGH | GPIO_OUTPUT_INIT_LOW)) != 0U) {
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_OUTPUT, output);
	}

	pull = (flags & GPIO_PULL_UP) != 0U ? 1U :
	       (flags & GPIO_PULL_DOWN) != 0U ? 2U : 0U;
	m5pm1_set_2bit_field(&pull0, &pull1, pin, pull);
	m5pm1_set_2bit_field(&func0, &func1, pin, M5PM1_FUNC_GPIO);
	WRITE_BIT(drive, pin, output_mode &&
		  ((flags & GPIO_OPEN_DRAIN) == GPIO_OPEN_DRAIN));
	WRITE_BIT(mode, pin, output_mode);

	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_PULL0, pull0);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_PULL1, pull1);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_DRIVE, drive);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_MODE, mode);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_FUNC0, func0);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_FUNC1, func1);
	if (ret == 0) {
		data->mode = mode;
		data->output = output;
		data->drive = drive;
		data->pull0 = pull0;
		data->pull1 = pull1;
		data->func0 = func0;
		data->func1 = func1;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5pm1_port_get_raw(const struct device *dev,
			      gpio_port_value_t *value)
{
	struct m5pm1_data *data = dev->data;
	uint8_t input;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_GPIO_INPUT, &input);
	if (ret == 0) {
		data->last_input = input;
	}
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*value = input & M5PM1_VALID_PINS;
	}
	return ret;
}

static int m5pm1_port_set_masked_raw(const struct device *dev,
				     gpio_port_pins_t mask,
				     gpio_port_value_t value)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	uint8_t output;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	mask &= config->common.port_pin_mask;
	k_mutex_lock(&data->lock, K_FOREVER);
	output = (data->output & ~mask) | (value & mask);
	ret = m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_OUTPUT, output);
	if (ret == 0) {
		data->output = output;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5pm1_port_set_bits_raw(const struct device *dev,
				   gpio_port_pins_t pins)
{
	return m5pm1_port_set_masked_raw(dev, pins, pins);
}

static int m5pm1_port_clear_bits_raw(const struct device *dev,
				     gpio_port_pins_t pins)
{
	return m5pm1_port_set_masked_raw(dev, pins, 0U);
}

static int m5pm1_port_toggle_bits(const struct device *dev,
				  gpio_port_pins_t pins)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	uint8_t output;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	pins &= config->common.port_pin_mask;
	k_mutex_lock(&data->lock, K_FOREVER);
	output = data->output ^ pins;
	ret = m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_OUTPUT, output);
	if (ret == 0) {
		data->output = output;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5pm1_pin_interrupt_configure(const struct device *dev,
					 gpio_pin_t pin,
					 enum gpio_int_mode mode,
					 enum gpio_int_trig trig)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	const uint8_t bit = BIT(pin);
	uint8_t mask;
	int ret;

	if (pin >= M5PM1_GPIO_COUNT ||
	    (bit & config->common.port_pin_mask) == 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (mode != GPIO_INT_MODE_DISABLED && mode != GPIO_INT_MODE_EDGE) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (mode != GPIO_INT_MODE_DISABLED && (data->mode & bit) != 0U) {
		ret = -ENOTSUP;
		goto out;
	}

	mask = data->irq_mask1;
	if (mode == GPIO_INT_MODE_DISABLED) {
		mask |= bit;
		data->edge_rising &= ~bit;
		data->edge_falling &= ~bit;
	} else {
		mask &= ~bit;
		WRITE_BIT(data->edge_rising, pin,
			  (trig & GPIO_INT_TRIG_HIGH) != 0U);
		WRITE_BIT(data->edge_falling, pin,
			  (trig & GPIO_INT_TRIG_LOW) != 0U);
	}
	ret = m5pm1_write8_unlocked(dev, M5PM1_REG_IRQ_STATUS1, 0U);
	ret = ret ?: m5pm1_write8_unlocked(dev, M5PM1_REG_IRQ_MASK1, mask);
	if (ret == 0) {
		data->irq_mask1 = mask;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int m5pm1_manage_callback(const struct device *dev,
				 struct gpio_callback *callback, bool set)
{
	struct m5pm1_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void m5pm1_interrupt_work_handler(struct k_work *work)
{
	struct m5pm1_data *data = CONTAINER_OF(
		work, struct m5pm1_data, interrupt_work);
	uint8_t wake = 0U;
	uint8_t status1 = 0U;
	uint8_t status2 = 0U;
	uint8_t status3 = 0U;
	uint8_t input = 0U;
	uint8_t triggered = 0U;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(data->self, M5PM1_REG_WAKE_SOURCE, &wake);
	ret = ret ?: m5pm1_read8_unlocked(
		data->self, M5PM1_REG_IRQ_STATUS1, &status1);
	ret = ret ?: m5pm1_read8_unlocked(
		data->self, M5PM1_REG_IRQ_STATUS2, &status2);
	ret = ret ?: m5pm1_read8_unlocked(
		data->self, M5PM1_REG_IRQ_STATUS3, &status3);
	ret = ret ?: m5pm1_read8_unlocked(
		data->self, M5PM1_REG_GPIO_INPUT, &input);
	if (ret == 0) {
		data->pending_irq.wake_source |= wake;
		data->pending_irq.gpio |= status1;
		data->pending_irq.system |= status2;
		data->pending_irq.button |= status3;
		triggered = status1 &
			((input & data->edge_rising) |
			 (~input & data->edge_falling));
		data->last_input = input;
		ret = m5pm1_write8_unlocked(
			data->self, M5PM1_REG_WAKE_SOURCE, 0U);
		ret = ret ?: m5pm1_write8_unlocked(
			data->self, M5PM1_REG_IRQ_STATUS1, 0U);
		ret = ret ?: m5pm1_write8_unlocked(
			data->self, M5PM1_REG_IRQ_STATUS2, 0U);
		ret = ret ?: m5pm1_write8_unlocked(
			data->self, M5PM1_REG_IRQ_STATUS3, 0U);
	}
	k_mutex_unlock(&data->lock);

	if (ret == 0 && triggered != 0U) {
		gpio_fire_callbacks(&data->callbacks, data->self, triggered);
	} else if (ret < 0) {
		LOG_ERR("Failed to service M5PM1 interrupt (%d)", ret);
	}
}

static void m5pm1_host_interrupt(const struct device *port,
				 struct gpio_callback *callback,
				 gpio_port_pins_t pins)
{
	struct m5pm1_data *data = CONTAINER_OF(
		callback, struct m5pm1_data, host_callback);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	k_work_submit(&data->interrupt_work);
}

int m5pm1_fetch_irq_status(const struct device *dev,
			   struct m5pm1_irq_status *status)
{
	struct m5pm1_data *data;

	if (dev == NULL || status == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	*status = data->pending_irq;
	memset(&data->pending_irq, 0, sizeof(data->pending_irq));
	k_mutex_unlock(&data->lock);
	return 0;
}

int m5pm1_clear_irq_status(const struct device *dev)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_write8_unlocked(dev, M5PM1_REG_WAKE_SOURCE, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS1, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS2, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS3, 0U);
	if (ret == 0) {
		memset(&data->pending_irq, 0, sizeof(data->pending_irq));
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_read_voltage(const struct device *dev, enum m5pm1_voltage source,
		       uint16_t *millivolts)
{
	static const uint8_t registers[] = {
		[M5PM1_VOLTAGE_REFERENCE] = M5PM1_REG_VREF_L,
		[M5PM1_VOLTAGE_BATTERY] = M5PM1_REG_VBAT_L,
		[M5PM1_VOLTAGE_INPUT] = M5PM1_REG_VIN_L,
		[M5PM1_VOLTAGE_5V_OUTPUT] = M5PM1_REG_5VOUT_L,
	};
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || millivolts == NULL ||
	    source > M5PM1_VOLTAGE_5V_OUTPUT) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read16_unlocked(dev, registers[source], millivolts);
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_read_adc(const struct device *dev, uint8_t channel,
		   uint16_t *sample)
{
	struct m5pm1_data *data;
	uint8_t control;
	uint16_t result;
	int ret;

	if (dev == NULL || sample == NULL ||
	    (channel != 1U && channel != 2U && channel != 6U)) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	control = (channel << 1) | 1U;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_write8_unlocked(dev, M5PM1_REG_ADC_CONTROL, control);
	for (uint8_t attempt = 0U; ret == 0 && attempt < 50U; ++attempt) {
		k_msleep(10);
		ret = m5pm1_read8_unlocked(
			dev, M5PM1_REG_ADC_CONTROL, &control);
		if (ret < 0 || (control & 1U) == 0U) {
			break;
		}
	}
	if (ret == 0 && (control & 1U) != 0U) {
		ret = -ETIMEDOUT;
	}
	ret = ret ?: m5pm1_read16_unlocked(
		dev, M5PM1_REG_ADC_RESULT_L, &result);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*sample = result & 0x0FFFU;
	}
	return ret;
}

int m5pm1_get_power_source(const struct device *dev, uint8_t *source)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || source == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_POWER_SOURCE, source);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*source &= 0x07U;
	}
	return ret;
}

int m5pm1_update_power(const struct device *dev, uint8_t mask, uint8_t value)
{
	struct m5pm1_data *data;
	uint8_t config;
	int ret;

	if (dev == NULL || (mask & ~M5PM1_POWER_VALID_MASK) != 0U) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_POWER_CONFIG, &config);
	if (ret == 0) {
		config = (config & ~mask) | (value & mask);
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_POWER_CONFIG, config);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_set_power_hold(const struct device *dev, uint8_t mask,
			 bool enable)
{
	struct m5pm1_data *data;
	uint8_t value;
	int ret;

	if (dev == NULL || mask == 0U ||
	    (mask & ~M5PM1_HOLD_VALID_MASK) != 0U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_HOLD_CONFIG, &value);
	if (ret == 0) {
		value = enable ? value | mask : value & ~mask;
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_HOLD_CONFIG, value);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_get_power_hold(const struct device *dev, uint8_t *mask)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || mask == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_HOLD_CONFIG, mask);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*mask &= M5PM1_HOLD_VALID_MASK;
	}
	return ret;
}

int m5pm1_configure_wake_gpio(const struct device *dev, uint8_t pin,
			      enum m5pm1_wake_edge edge, bool enable)
{
	struct m5pm1_data *data;
	uint8_t wake_enable;
	uint8_t wake_edge;
	int ret;

	if (dev == NULL || pin >= M5PM1_GPIO_COUNT || pin == 1U ||
	    edge > M5PM1_WAKE_RISING) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_EN, &wake_enable);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_CFG, &wake_edge);
	if (ret < 0) {
		goto out;
	}
	/* M5PM1 exposes two mutually-exclusive WAKE pairs: GPIO0/GPIO2 and
	 * GPIO3/GPIO4.  This is a PMIC wake-mux restriction, not an IRQ-mask
	 * mapping; each selected GPIO still uses its own register bit below.
	 */
	if (enable && ((pin == 0U && (wake_enable & BIT(2)) != 0U) ||
		       (pin == 2U && (wake_enable & BIT(0)) != 0U) ||
		       (pin == 3U && (wake_enable & BIT(4)) != 0U) ||
		       (pin == 4U && (wake_enable & BIT(3)) != 0U))) {
		ret = -EBUSY;
		goto out;
	}
	WRITE_BIT(wake_edge, pin, edge == M5PM1_WAKE_RISING);
	WRITE_BIT(wake_enable, pin, enable);
	ret = m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_CFG, wake_edge);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_EN, wake_enable);
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_read_wake_source(const struct device *dev, uint8_t *source,
			   bool clear)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || source == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_WAKE_SOURCE, source);
	if (ret == 0 && clear) {
		ret = m5pm1_write8_unlocked(dev, M5PM1_REG_WAKE_SOURCE, 0U);
	}
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*source &= 0x7FU;
	}
	return ret;
}

int m5pm1_get_boot_wake_source(const struct device *dev, uint8_t *source)
{
	struct m5pm1_data *data;

	if (dev == NULL || source == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	*source = data->boot_wake_source;
	k_mutex_unlock(&data->lock);
	return 0;
}

int m5pm1_get_boot_wake_gpio_mask(const struct device *dev, uint8_t *mask)
{
	struct m5pm1_data *data;

	if (dev == NULL || mask == NULL) {
		return -EINVAL;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	*mask = data->boot_wake_gpio_mask & M5PM1_VALID_PINS;
	k_mutex_unlock(&data->lock);
	return 0;
}

int m5pm1_set_i2c_sleep_timeout(const struct device *dev, uint8_t seconds)
{
	struct m5pm1_data *data;
	uint8_t config;
	uint8_t actual;
	int ret;

	if (dev == NULL || seconds > 15U) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_I2C_CONFIG, &config);
	if (ret == 0) {
		config = (config & ~0x0FU) | seconds;
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_I2C_CONFIG, config);
	}
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_I2C_CONFIG, &actual);
	if (ret == 0 && (actual & 0x0FU) != seconds) {
		ret = -EIO;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_get_i2c_sleep_timeout(const struct device *dev, uint8_t *seconds)
{
	struct m5pm1_data *data;
	int ret;

	if (dev == NULL || seconds == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(dev, M5PM1_REG_I2C_CONFIG, seconds);
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		*seconds &= 0x0FU;
	}
	return ret;
}

int m5pm1_send_wake_signal(const struct device *dev)
{
	const struct m5pm1_config *config;
	struct m5pm1_data *data;
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
	/* An address-only write is the PM1 wake pulse. A normal register read
	 * immediately afterwards confirms that it has resumed.
	 */
	ret = i2c_write(config->bus.bus, NULL, 0U, config->bus.addr);
	if (ret == 0) {
		k_busy_wait(500);
		uint8_t revision;

		ret = m5pm1_read8_unlocked(dev, 0x02U, &revision);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_set_battery_lvp(const struct device *dev, uint16_t millivolts)
{
	uint32_t raw;
	uint8_t value;

	if (dev == NULL || millivolts < 2000U || millivolts > 4000U) {
		return -EINVAL;
	}
	raw = DIV_ROUND_CLOSEST((uint32_t)(millivolts - 2000U) * 100U,
				781U);
	value = MIN(raw, UINT8_MAX);
	return m5pm1_reg_write(
		dev, M5PM1_REG_BATTERY_LVP, &value, sizeof(value));
}

int m5pm1_rtc_ram_read(const struct device *dev, uint8_t offset,
			void *buffer, size_t length)
{
	if (offset >= M5PM1_RTC_RAM_SIZE || length == 0U ||
	    length > M5PM1_RTC_RAM_SIZE - offset) {
		return -EINVAL;
	}
	return m5pm1_reg_read(
		dev, M5PM1_REG_RTC_RAM + offset, buffer, length);
}

int m5pm1_rtc_ram_write(const struct device *dev, uint8_t offset,
			 const void *buffer, size_t length, bool verify)
{
	uint8_t actual[M5PM1_RTC_RAM_SIZE];
	int ret;

	if (buffer == NULL || offset >= M5PM1_RTC_RAM_SIZE || length == 0U ||
	    length > M5PM1_RTC_RAM_SIZE - offset) {
		return -EINVAL;
	}
	ret = m5pm1_reg_write(
		dev, M5PM1_REG_RTC_RAM + offset, buffer, length);
	if (ret == 0 && verify) {
		ret = m5pm1_rtc_ram_read(dev, offset, actual, length);
		if (ret == 0 && memcmp(actual, buffer, length) != 0) {
			ret = -EIO;
		}
	}
	return ret;
}

int m5pm1_set_status_led(const struct device *dev, bool enabled,
			 bool open_drain)
{
	struct m5pm1_data *data;
	uint8_t power;
	uint8_t drive;
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	drive = data->drive;
	WRITE_BIT(drive, 5U, open_drain);
	ret = 0;
	if (drive != data->drive) {
		ret = m5pm1_write8_unlocked(dev, M5PM1_REG_GPIO_DRIVE, drive);
		if (ret == 0) {
			data->drive = drive;
		} else {
			LOG_ERR("status LED drive update failed (%d)", ret);
		}
	}
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_POWER_CONFIG, &power);
	if (ret == 0) {
		WRITE_BIT(power, 4U, enabled);
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_POWER_CONFIG, power);
		if (ret < 0) {
			LOG_ERR("status LED level update failed (%d)", ret);
		}
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_set_watchdog(const struct device *dev, uint8_t timeout_seconds)
{
	return m5pm1_reg_write(dev, M5PM1_REG_WATCHDOG_COUNT,
			      &timeout_seconds, sizeof(timeout_seconds));
}

int m5pm1_feed_watchdog(const struct device *dev)
{
	const uint8_t key = M5PM1_WATCHDOG_FEED_KEY;

	return m5pm1_reg_write(dev, M5PM1_REG_WATCHDOG_KEY, &key, sizeof(key));
}

int m5pm1_pwm_set(const struct device *dev, uint8_t channel,
		  uint16_t frequency_hz, uint16_t duty_12bit,
		  bool inverted, bool enable)
{
	struct m5pm1_data *data;
	const uint8_t channel_bit = BIT(channel);
	uint8_t other_enabled;
	uint8_t duty[2];
	uint8_t mode;
	uint8_t drive;
	uint8_t pull0;
	uint8_t pull1;
	uint8_t func0;
	uint8_t func1;
	uint8_t pin;
	int ret = 0;

	if (dev == NULL || channel >= M5PM1_PWM_COUNT ||
	    duty_12bit > 0x0FFFU || (enable && frequency_hz == 0U)) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	data = dev->data;
	pin = m5pm1_pwm_pins[channel];
	k_mutex_lock(&data->lock, K_FOREVER);
	other_enabled = data->pwm_enabled & ~channel_bit;
	if (enable && other_enabled != 0U &&
	    data->pwm_frequency != frequency_hz) {
		ret = -EBUSY;
		goto out;
	}

	if (enable && data->pwm_frequency != frequency_hz) {
		ret = m5pm1_write16_unlocked(
			dev, M5PM1_REG_PWM_FREQUENCY, frequency_hz);
		if (ret < 0) {
			goto out;
		}
		data->pwm_frequency = frequency_hz;
	}

	duty[0] = (uint8_t)duty_12bit;
	duty[1] = (uint8_t)(duty_12bit >> 8);
	if (inverted) {
		duty[1] |= M5PM1_PWM_POLARITY;
	}
	if (enable) {
		duty[1] |= M5PM1_PWM_ENABLE;
	}
	ret = m5pm1_write_unlocked(
		dev, M5PM1_REG_PWM0_L + channel * 2U, duty, sizeof(duty));
	if (ret < 0) {
		goto out;
	}
	WRITE_BIT(data->pwm_enabled, channel, enable);

	if (enable) {
		mode = data->mode | BIT(pin);
		drive = data->drive & ~BIT(pin);
		pull0 = data->pull0;
		pull1 = data->pull1;
		func0 = data->func0;
		func1 = data->func1;
		m5pm1_set_2bit_field(&pull0, &pull1, pin, 0U);
		m5pm1_set_2bit_field(
			&func0, &func1, pin, M5PM1_FUNC_OTHER);
		ret = m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_PULL0, pull0);
		ret = ret ?: m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_PULL1, pull1);
		ret = ret ?: m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_DRIVE, drive);
		ret = ret ?: m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_MODE, mode);
		ret = ret ?: m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_FUNC0, func0);
		ret = ret ?: m5pm1_write8_unlocked(
			dev, M5PM1_REG_GPIO_FUNC1, func1);
		if (ret == 0) {
			data->mode = mode;
			data->drive = drive;
			data->pull0 = pull0;
			data->pull1 = pull1;
			data->func0 = func0;
			data->func1 = func1;
		}
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int m5pm1_system_command(const struct device *dev,
			 enum m5pm1_system_command command)
{
	uint8_t value;

	if (dev == NULL || command < M5PM1_SYSTEM_SHUTDOWN ||
	    command > M5PM1_SYSTEM_DOWNLOAD) {
		return -EINVAL;
	}
	k_msleep(120);
	value = M5PM1_SYSTEM_KEY | (uint8_t)command;
	return m5pm1_reg_write(
		dev, M5PM1_REG_SYSTEM_COMMAND, &value, sizeof(value));
}

static DEVICE_API(gpio, m5pm1_gpio_api) = {
	.pin_configure = m5pm1_pin_configure,
	.port_get_raw = m5pm1_port_get_raw,
	.port_set_masked_raw = m5pm1_port_set_masked_raw,
	.port_set_bits_raw = m5pm1_port_set_bits_raw,
	.port_clear_bits_raw = m5pm1_port_clear_bits_raw,
	.port_toggle_bits = m5pm1_port_toggle_bits,
	.pin_interrupt_configure = m5pm1_pin_interrupt_configure,
	.manage_callback = m5pm1_manage_callback,
};

static int m5pm1_configure_irq_output(const struct device *dev)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	const uint8_t bit = BIT(config->irq_output_pin);
	int ret;

	data->output |= bit;
	data->mode |= bit;
	data->drive &= ~bit;
	m5pm1_set_2bit_field(
		&data->pull0, &data->pull1, config->irq_output_pin, 1U);
	m5pm1_set_2bit_field(
		&data->func0, &data->func1, config->irq_output_pin,
		M5PM1_FUNC_IRQ);
	data->irq_mask1 = M5PM1_VALID_PINS;

	ret = m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_OUTPUT, data->output);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_MODE, data->mode);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_DRIVE, data->drive);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_PULL0, data->pull0);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_PULL1, data->pull1);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_FUNC0, data->func0);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_FUNC1, data->func1);
	/* Wake enables survive an M5PM1-controlled shutdown.  Disable every
	 * external wake input before clearing the latched source and IRQ status,
	 * otherwise an asserted RTC/IMU line can immediately retrigger the PMIC
	 * while Zephyr is still booting.
	 */
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_EN, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_MASK1, data->irq_mask1);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_WAKE_SOURCE, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS1, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS2, 0U);
	ret = ret ?: m5pm1_write8_unlocked(
		dev, M5PM1_REG_IRQ_STATUS3, 0U);
	return ret;
}

static int m5pm1_init(const struct device *dev)
{
	const struct m5pm1_config *config = dev->config;
	struct m5pm1_data *data = dev->data;
	struct m5pm1_info info;
	uint8_t zero = 0U;
	uint8_t power_config;
	uint8_t hold_config;
	uint16_t pwm;
	unsigned int attempt;
	int ret;

	BUILD_ASSERT(DT_INST_PROP(0, irq_output_pin) < M5PM1_GPIO_COUNT);
	if (!i2c_is_ready_dt(&config->bus) ||
	    !gpio_is_ready_dt(&config->interrupt)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	sys_slist_init(&data->callbacks);
	k_work_init(&data->interrupt_work, m5pm1_interrupt_work_handler);
	data->self = dev;

	/* Retry immediately: the first START can wake M5PM1 from I2C sleep, while
	 * a fixed boot delay leaves too much time for a stale wake input to
	 * retrigger after an M5PM1-controlled shutdown.
	 */
	for (attempt = 0U; attempt < 8U; ++attempt) {
		ret = m5pm1_get_info(dev, &info);
		if (ret == 0) {
			break;
		}

		(void)i2c_recover_bus(config->bus.bus);
		k_msleep(50);
	}
	if (ret < 0) {
		LOG_ERR("M5PM1 at 0x%02x is not responding after %u attempts (%d)",
			config->bus.addr, attempt, ret);
		return ret;
	}
	ret = m5pm1_reg_write(
		dev, M5PM1_REG_I2C_CONFIG, &zero, sizeof(zero));
	ret = ret ?: m5pm1_reg_write(
		dev, M5PM1_REG_WATCHDOG_COUNT, &zero, sizeof(zero));
	if (ret < 0) {
		return ret;
	}
	k_msleep(2);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = m5pm1_read8_unlocked(
		dev, M5PM1_REG_POWER_CONFIG, &power_config);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_HOLD_CONFIG, &hold_config);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_WAKE_SOURCE, &data->boot_wake_source);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_WAKE_EN, &data->boot_wake_gpio_mask);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_MODE, &data->mode);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_OUTPUT, &data->output);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_DRIVE, &data->drive);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_PULL0, &data->pull0);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_PULL1, &data->pull1);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_FUNC0, &data->func0);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_FUNC1, &data->func1);
	ret = ret ?: m5pm1_read8_unlocked(
		dev, M5PM1_REG_GPIO_INPUT, &data->last_input);
	ret = ret ?: m5pm1_read16_unlocked(
		dev, M5PM1_REG_PWM_FREQUENCY, &data->pwm_frequency);
	for (uint8_t channel = 0U;
	     ret == 0 && channel < M5PM1_PWM_COUNT; ++channel) {
		ret = m5pm1_read16_unlocked(
			dev, M5PM1_REG_PWM0_L + channel * 2U, &pwm);
		if (ret == 0 &&
		    (pwm & ((uint16_t)M5PM1_PWM_ENABLE << 8)) != 0U) {
			data->pwm_enabled |= BIT(channel);
		}
	}
	ret = ret ?: m5pm1_configure_irq_output(dev);
	k_mutex_unlock(&data->lock);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_configure_dt(
		&config->interrupt, GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		return ret;
	}
	gpio_init_callback(&data->host_callback, m5pm1_host_interrupt,
			   BIT(config->interrupt.pin));
	ret = gpio_add_callback(
		config->interrupt.port, &data->host_callback);
	ret = ret ?: gpio_pin_interrupt_configure_dt(
		&config->interrupt, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("M5PM1 id=0x%02x model=0x%02x hw=0x%02x sw=0x%02x ready; "
		"power=0x%02x hold=0x%02x wake=0x%02x gpio_wake=0x%02x",
		info.id, info.model, info.hardware_revision,
		info.software_revision, power_config, hold_config,
		data->boot_wake_source, data->boot_wake_gpio_mask);
	return 0;
}

#define M5PM1_DEFINE(inst)                                                   \
	static const struct m5pm1_config m5pm1_config_##inst = {             \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(inst),                 \
		.bus = I2C_DT_SPEC_INST_GET(inst),                               \
		.interrupt = GPIO_DT_SPEC_INST_GET(inst, interrupt_gpios),       \
		.irq_output_pin = DT_INST_PROP(inst, irq_output_pin),            \
	};                                                                        \
	static struct m5pm1_data m5pm1_data_##inst;                              \
	DEVICE_DT_INST_DEFINE(inst, m5pm1_init, NULL, &m5pm1_data_##inst,         \
			      &m5pm1_config_##inst, POST_KERNEL,                 \
			      CONFIG_PAPERMONO_MFD_INIT_PRIORITY, &m5pm1_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(M5PM1_DEFINE)
