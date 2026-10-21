/* SPDX-License-Identifier: GPL-3.0-only */

#define DT_DRV_COMPAT m5stack_papermono_ip2315

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <papermono/m5ioe1.h>
#include <papermono/m5pm1.h>

LOG_MODULE_REGISTER(ip2315, CONFIG_CHARGER_LOG_LEVEL);

#define IP2316_REG_CHARGE_CONTROL 0x01U
#define IP2316_REG_CHARGE_STATUS  0xC7U
#define IP2316_CHARGE_ENABLE      BIT(0)
#define IP2316_STATUS_CHARGING    BIT(7)
#define IP2316_STATUS_COMPLETE    0x45U
#define IP2316_GATE_SETTLE_MS     10U

struct ip2316_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec enable;
	const struct device *pmic;
};

struct ip2316_data {
	struct k_mutex lock;
};

static int ip2316_begin(const struct device *dev, uint8_t *status)
{
	const struct ip2316_config *config = dev->config;
	uint8_t output_bytes[2] = {0U, 0U};
	uint16_t output_latch;
	int gate_level;
	int probe_ret = -ENXIO;
	int recover_ret;
	int ret;

	ret = gpio_pin_set_dt(&config->enable, 1);
	if (ret < 0) {
		return ret;
	}
	/* The IP2315 I2C interface is physically connected only while G11 is
	 * high. M5Unified waits two milliseconds and then polls repeatedly, but
	 * every NACK can consume the full Zephyr ESP32 I2C timeout. Give the gate
	 * a conservative fixed settle interval and perform one bounded probe so
	 * an absent/non-responsive charger cannot stall the UI or power HIL for
	 * minutes.
	 */
	k_msleep(IP2316_GATE_SETTLE_MS);
	probe_ret = i2c_write_dt(&config->bus, NULL, 0U);
	ret = probe_ret;
	if (ret < 0) {
		/* A NACKed address phase does not reliably leave the ESP32-S3
		 * controller ready for the next slave. Restore the shared bus before
		 * accessing M5IOE1 again to close the charger gate.
		 */
		recover_ret = i2c_recover_bus(config->bus.bus);
		if (recover_ret < 0) {
			LOG_ERR("shared I2C recovery after charger probe failed (%d)",
				recover_ret);
		} else {
			LOG_WRN("shared I2C recovered after charger probe failure");
		}
	}
	if (ret == 0) {
		ret = i2c_reg_read_byte_dt(
			&config->bus, IP2316_REG_CHARGE_STATUS, status);
		if (ret < 0) {
			LOG_ERR("charger ACKed address but status read failed (%d)", ret);
		}
	}
	if (ret < 0) {
		gate_level = gpio_pin_get_dt(&config->enable);
		(void)m5ioe1_reg_read(config->enable.port, 0x05U,
				       output_bytes, sizeof(output_bytes));
		output_latch = sys_get_le16(output_bytes);
		LOG_ERR("charger did not respond (probe=%d gate-in=%d gate-latch=%d ret=%d)",
			probe_ret,
			gate_level,
			(output_latch & BIT(config->enable.pin)) != 0U, ret);
		(void)gpio_pin_set_dt(&config->enable, 0);
	}
	return ret;
}

static int ip2316_end(const struct device *dev, int operation_ret)
{
	const struct ip2316_config *config = dev->config;
	const int gate_ret = gpio_pin_set_dt(&config->enable, 0);

	return operation_ret < 0 ? operation_ret : gate_ret;
}

static int ip2316_get_property(const struct device *dev, charger_prop_t prop,
			       union charger_propval *value)
{
	const struct ip2316_config *config = dev->config;
	struct ip2316_data *data = dev->data;
	uint8_t source;
	uint8_t status;
	int ret;

	if (value == NULL) {
		return -EINVAL;
	}

	ret = m5pm1_get_power_source(config->pmic, &source);
	if (ret < 0) {
		return ret;
	}
	if (prop == CHARGER_PROP_ONLINE) {
		value->online = (source & 0x03U) == 2U ?
			CHARGER_ONLINE_OFFLINE : CHARGER_ONLINE_FIXED;
		return 0;
	}
	if (prop == CHARGER_PROP_STATUS && (source & 0x03U) == 2U) {
		value->status = CHARGER_STATUS_DISCHARGING;
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ip2316_begin(dev, &status);
	if (ret == 0) {
		switch (prop) {
		case CHARGER_PROP_STATUS:
			value->status = (status & IP2316_STATUS_CHARGING) != 0U ?
				CHARGER_STATUS_CHARGING :
				status == IP2316_STATUS_COMPLETE ?
				CHARGER_STATUS_FULL : CHARGER_STATUS_NOT_CHARGING;
			break;
		case CHARGER_PROP_CHARGE_TYPE:
			value->charge_type =
				(status & IP2316_STATUS_CHARGING) != 0U ?
				CHARGER_CHARGE_TYPE_FAST : CHARGER_CHARGE_TYPE_NONE;
			break;
		case CHARGER_PROP_HEALTH:
			value->health = CHARGER_HEALTH_GOOD;
			break;
		default:
			ret = -ENOTSUP;
			break;
		}
	}
	ret = ip2316_end(dev, ret);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int ip2316_set_property(const struct device *dev, charger_prop_t prop,
			       const union charger_propval *value)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(prop);
	ARG_UNUSED(value);
	return -ENOTSUP;
}

static int ip2316_charge_enable(const struct device *dev, bool enable)
{
	const struct ip2316_config *config = dev->config;
	struct ip2316_data *data = dev->data;
	uint8_t status;
	uint8_t control;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ip2316_begin(dev, &status);
	ret = ret ?: i2c_reg_read_byte_dt(
		&config->bus, IP2316_REG_CHARGE_CONTROL, &control);
	if (ret == 0) {
		WRITE_BIT(control, 0, enable);
		ret = i2c_reg_write_byte_dt(
			&config->bus, IP2316_REG_CHARGE_CONTROL, control);
	}
	ret = ip2316_end(dev, ret);
	k_mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_API(charger, ip2316_api) = {
	.get_property = ip2316_get_property,
	.set_property = ip2316_set_property,
	.charge_enable = ip2316_charge_enable,
};

static int ip2316_init(const struct device *dev)
{
	const struct ip2316_config *config = dev->config;
	struct ip2316_data *data = dev->data;

	if (!i2c_is_ready_dt(&config->bus) ||
	    !gpio_is_ready_dt(&config->enable) ||
	    !device_is_ready(config->pmic)) {
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	return gpio_pin_configure_dt(&config->enable, GPIO_OUTPUT_INACTIVE);
}

#define IP2316_DEFINE(inst)                                                   \
	static const struct ip2316_config ip2316_config_##inst = {             \
		.bus = I2C_DT_SPEC_INST_GET(inst),                              \
		.enable = GPIO_DT_SPEC_INST_GET(inst, enable_gpios),             \
		.pmic = DEVICE_DT_GET(DT_INST_PHANDLE(inst, pmic)),               \
	};                                                                         \
	static struct ip2316_data ip2316_data_##inst;                              \
	DEVICE_DT_INST_DEFINE(inst, ip2316_init, NULL, &ip2316_data_##inst,         \
			      &ip2316_config_##inst, POST_KERNEL,                   \
			      CONFIG_PAPERMONO_BOARD_FUNCTION_INIT_PRIORITY,        \
			      &ip2316_api);

DT_INST_FOREACH_STATUS_OKAY(IP2316_DEFINE)
