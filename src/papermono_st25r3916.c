/* SPDX-License-Identifier: GPL-3.0-only */

#define DT_DRV_COMPAT m5stack_papermono_st25r3916

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <papermono/st25r3916.h>

LOG_MODULE_REGISTER(papermono_st25r3916, CONFIG_PAPERMONO_ST25R3916_LOG_LEVEL);

#define ST25R3916_REG_IDENTITY         0x3FU
#define ST25R3916_REGISTER_ADDRESS_MASK 0x3FU
#define ST25R3916_REGISTER_READ         0x40U
#define ST25R3916_EXPECTED_TYPE         0x05U
#define ST25R3916_IDENTITY_RETRIES      5U
#define ST25R3916_IDENTITY_RETRY_MS     20U
#define ST25R3916_MAX_BURST             64U

/* ST25R3916 register space A. */
#define ST25R3916_REG_IO_CONFIG_1        0x00U
#define ST25R3916_REG_IO_CONFIG_2        0x01U
#define ST25R3916_REG_OPERATION_CONTROL  0x02U
#define ST25R3916_REG_MODE_DEFINITION    0x03U
#define ST25R3916_REG_BITRATE_DEFINITION 0x04U
#define ST25R3916_REG_ISO14443A_SETTINGS 0x05U
#define ST25R3916_REG_ISO14443B_SETTINGS 0x06U
#define ST25R3916_REG_NFCIP1_TARGET      0x08U
#define ST25R3916_REG_STREAM_MODE        0x09U
#define ST25R3916_REG_AUX_DEFINITION     0x0AU
#define ST25R3916_REG_RX_CONFIG_1        0x0BU
#define ST25R3916_REG_RX_CONFIG_2        0x0CU
#define ST25R3916_REG_RX_CONFIG_3        0x0DU
#define ST25R3916_REG_RX_CONFIG_4        0x0EU
#define ST25R3916_REG_NO_RESPONSE_1      0x10U
#define ST25R3916_REG_TIMER_CONTROL      0x12U
#define ST25R3916_REG_GPT_1              0x13U
#define ST25R3916_REG_GPT_2              0x14U
#define ST25R3916_REG_PPON2              0x15U
#define ST25R3916_REG_MASK_MAIN_IRQ      0x16U
#define ST25R3916_REG_MASK_TIMER_IRQ     0x17U
#define ST25R3916_REG_MASK_ERROR_IRQ     0x18U
#define ST25R3916_REG_MASK_PASSIVE_IRQ   0x19U
#define ST25R3916_REG_MAIN_IRQ           0x1AU
#define ST25R3916_REG_ERROR_IRQ          0x1CU
#define ST25R3916_REG_PASSIVE_IRQ        0x1DU
#define ST25R3916_REG_FIFO_STATUS_1      0x1EU
#define ST25R3916_REG_COLLISION_DISPLAY  0x20U
#define ST25R3916_REG_TX_BYTES_1         0x22U
#define ST25R3916_REG_ANT_TUNE_1         0x26U
#define ST25R3916_REG_TX_DRIVER          0x28U
#define ST25R3916_REG_TARGET_MODULATION  0x29U
#define ST25R3916_REG_FIELD_ON_THRESHOLD 0x2AU
#define ST25R3916_REG_FIELD_OFF_THRESHOLD 0x2BU
#define ST25R3916_REG_REGULATOR_CONTROL  0x2CU
#define ST25R3916_REG_AUX_DISPLAY        0x31U

/* Register space B addresses. */
#define ST25R3916_REG_B_EMD_SUPPRESSION  0x05U
#define ST25R3916_REG_B_SUBCARRIER_TIMER 0x06U
#define ST25R3916_REG_B_P2P_RX_CONFIG    0x0BU
#define ST25R3916_REG_B_CORRELATOR_1     0x0CU
#define ST25R3916_REG_B_CORRELATOR_2     0x0DU
#define ST25R3916_REG_B_SQUELCH_TIMER    0x0FU
#define ST25R3916_REG_B_FIELD_GUARD      0x15U
#define ST25R3916_REG_B_AUX_MODULATION   0x28U
#define ST25R3916_REG_B_TX_TIMING        0x29U
#define ST25R3916_REG_B_RESISTIVE_AM     0x2AU
#define ST25R3916_REG_B_REGULATOR_DISPLAY 0x2CU
#define ST25R3916_REG_B_OVERSHOOT_1      0x30U
#define ST25R3916_REG_B_OVERSHOOT_2      0x31U
#define ST25R3916_REG_B_UNDERSHOOT_1     0x32U
#define ST25R3916_REG_B_UNDERSHOOT_2     0x33U

/* Serial operations and direct commands. */
#define ST25R3916_OP_LOAD_FIFO           0x80U
#define ST25R3916_OP_READ_FIFO           0x9FU
#define ST25R3916_CMD_SET_DEFAULT        0xC1U
#define ST25R3916_CMD_STOP_ALL           0xC2U
#define ST25R3916_CMD_TRANSMIT_CRC       0xC4U
#define ST25R3916_CMD_TRANSMIT_NO_CRC    0xC5U
#define ST25R3916_CMD_TRANSMIT_REQA      0xC6U
#define ST25R3916_CMD_INITIAL_FIELD_ON   0xC8U
#define ST25R3916_CMD_RESET_RX_GAIN      0xD5U
#define ST25R3916_CMD_ADJUST_REGULATORS  0xD6U
#define ST25R3916_CMD_CLEAR_FIFO         0xDBU
#define ST25R3916_CMD_SPACE_B            0xFBU
#define ST25R3916_CMD_TEST_ACCESS        0xFCU

#define ST25R3916_OP_CTRL_ENABLE         BIT(7)
#define ST25R3916_OP_CTRL_RX_ENABLE      BIT(6)
#define ST25R3916_OP_CTRL_TX_ENABLE      BIT(3)
#define ST25R3916_AUX_OSC_OK             BIT(4)
#define ST25R3916_AUX_NO_CRC_RX          BIT(7)
#define ST25R3916_ISOA_ANTICOLLISION     BIT(0)
#define ST25R3916_SAK_CASCADE            BIT(2)
#define ST25R3916_CASCADE_TAG            0x88U

#define ST25R3916_IRQ_OSC                ((uint32_t)BIT(7) << 24)
#define ST25R3916_IRQ_RX_START           ((uint32_t)BIT(5) << 24)
#define ST25R3916_IRQ_RX_END             ((uint32_t)BIT(4) << 24)
#define ST25R3916_IRQ_COLLISION          ((uint32_t)BIT(2) << 24)
#define ST25R3916_IRQ_NO_RESPONSE        ((uint32_t)BIT(6) << 16)

#define ST25R3916_NFCA_REQA_TIMEOUT_MS   4U
#define ST25R3916_NFCA_FRAME_TIMEOUT_MS  8U
#define ST25R3916_NFCB_TIMEOUT_MS        5U
#define ST25R3916_NFCF_TIMEOUT_MS        12U
#define ST25R3916_NFCV_TIMEOUT_MS        16U

enum st25r3916_rf_mode {
	ST25R3916_RF_NONE,
	ST25R3916_RF_A,
	ST25R3916_RF_B,
	ST25R3916_RF_F,
	ST25R3916_RF_V,
};

struct papermono_st25r3916_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec power;
	struct gpio_dt_spec irq;
	uint16_t power_on_delay_ms;
};

struct papermono_st25r3916_data {
	struct k_mutex lock;
	struct gpio_callback irq_gpio_callback;
	const struct device *dev;
	papermono_st25r3916_irq_callback_t irq_callback;
	void *irq_user_data;
	struct papermono_st25r3916_info info;
	bool powered;
	enum st25r3916_rf_mode rf_mode;
};

static int st25r3916_read_unlocked(const struct device *dev, uint8_t reg,
				   uint8_t *data, size_t length);

static int st25r3916_write_unlocked(const struct device *dev, uint8_t reg,
				    const uint8_t *value, size_t length)
{
	const struct papermono_st25r3916_config *config = dev->config;
	uint8_t buffer[ST25R3916_MAX_BURST + 1U];
	int ret;

	if (length > ST25R3916_MAX_BURST) {
		return -EMSGSIZE;
	}
	buffer[0] = reg & ST25R3916_REGISTER_ADDRESS_MASK;
	if (length != 0U) {
		memcpy(&buffer[1], value, length);
	}
	ret = i2c_write_dt(&config->bus, buffer, length + 1U);
	if (ret == -EFAULT || ret == -ETIMEDOUT || ret == -EBUSY) {
		LOG_WRN("I2C register write 0x%02x failed (%d); recovering bus",
			reg, ret);
		if (i2c_recover_bus(config->bus.bus) == 0) {
			ret = i2c_write_dt(&config->bus, buffer, length + 1U);
		}
	}
	if (ret < 0) {
		LOG_ERR("I2C register write 0x%02x len=%u failed (%d)",
			reg, (unsigned int)length, ret);
	}
	return ret;
}

static int st25r3916_write8_unlocked(const struct device *dev, uint8_t reg,
				     uint8_t value)
{
	return st25r3916_write_unlocked(dev, reg, &value, sizeof(value));
}

static int st25r3916_write16_be_unlocked(const struct device *dev,
					 uint8_t reg, uint16_t value)
{
	uint8_t bytes[2];

	sys_put_be16(value, bytes);
	return st25r3916_write_unlocked(dev, reg, bytes, sizeof(bytes));
}

static int st25r3916_command_write_unlocked(const struct device *dev,
					    uint8_t command,
					    const uint8_t *payload,
					    size_t length)
{
	const struct papermono_st25r3916_config *config = dev->config;
	uint8_t buffer[ST25R3916_MAX_BURST + 1U];
	int ret;

	if (length > ST25R3916_MAX_BURST ||
	    (length != 0U && payload == NULL)) {
		return -EINVAL;
	}
	buffer[0] = command;
	if (length != 0U) {
		memcpy(&buffer[1], payload, length);
	}
	ret = i2c_write_dt(&config->bus, buffer, length + 1U);
	if (ret == -EFAULT || ret == -ETIMEDOUT || ret == -EBUSY) {
		LOG_WRN("I2C command 0x%02x failed (%d); recovering bus",
			command, ret);
		if (i2c_recover_bus(config->bus.bus) == 0) {
			ret = i2c_write_dt(&config->bus, buffer, length + 1U);
		}
	}
	if (ret < 0) {
		LOG_ERR("I2C command 0x%02x len=%u failed (%d)", command,
			(unsigned int)length, ret);
	}
	return ret;
}

static int st25r3916_command_read_unlocked(const struct device *dev,
					   uint8_t command,
					   uint8_t *value, size_t length)
{
	const struct papermono_st25r3916_config *config = dev->config;
	int ret;

	ret = i2c_write_read_dt(&config->bus, &command, sizeof(command),
				value, length);
	if (ret == -EFAULT || ret == -ETIMEDOUT || ret == -EBUSY) {
		LOG_WRN("I2C command read 0x%02x failed (%d); recovering bus",
			command, ret);
		if (i2c_recover_bus(config->bus.bus) == 0) {
			ret = i2c_write_read_dt(&config->bus, &command,
						sizeof(command), value, length);
		}
	}
	return ret;
}

static int st25r3916_space_b_write_unlocked(const struct device *dev,
					    uint8_t reg, uint8_t value)
{
	const uint8_t payload[2] = {
		reg & ST25R3916_REGISTER_ADDRESS_MASK,
		value,
	};

	return st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_SPACE_B, payload, sizeof(payload));
}

static int st25r3916_space_b_read_unlocked(const struct device *dev,
					   uint8_t reg, uint8_t *value)
{
	const struct papermono_st25r3916_config *config = dev->config;
	const uint8_t command[2] = {
		ST25R3916_CMD_SPACE_B,
		ST25R3916_REGISTER_READ |
			(reg & ST25R3916_REGISTER_ADDRESS_MASK),
	};
	int ret;

	ret = i2c_write_read_dt(&config->bus, command, sizeof(command),
				value, sizeof(*value));
	if (ret == -EFAULT || ret == -ETIMEDOUT || ret == -EBUSY) {
		LOG_WRN("I2C space-B read 0x%02x failed (%d); recovering bus",
			reg, ret);
		if (i2c_recover_bus(config->bus.bus) == 0) {
			ret = i2c_write_read_dt(&config->bus, command,
						sizeof(command), value, sizeof(*value));
		}
	}
	return ret;
}

static int st25r3916_update8_unlocked(const struct device *dev, uint8_t reg,
				      uint8_t set, uint8_t clear)
{
	uint8_t value;
	int ret;

	ret = st25r3916_read_unlocked(dev, reg, &value, sizeof(value));
	if (ret == 0) {
		value = (value & ~clear) | set;
		ret = st25r3916_write8_unlocked(dev, reg, value);
	}
	return ret;
}

static int st25r3916_read_interrupts_unlocked(const struct device *dev,
					      uint32_t *interrupts)
{
	uint8_t error = 0U;
	uint8_t main_nfc[2] = {0U, 0U};
	uint8_t passive = 0U;
	int ret;

	/* Reading MAIN clears the error register, so capture ERROR first. */
	ret = st25r3916_read_unlocked(
		dev, ST25R3916_REG_ERROR_IRQ, &error, sizeof(error));
	ret = ret ?: st25r3916_read_unlocked(
		dev, ST25R3916_REG_MAIN_IRQ, main_nfc, sizeof(main_nfc));
	ret = ret ?: st25r3916_read_unlocked(
		dev, ST25R3916_REG_PASSIVE_IRQ, &passive, sizeof(passive));
	if (ret == 0) {
		*interrupts = ((uint32_t)main_nfc[0] << 24) |
			((uint32_t)main_nfc[1] << 16) |
			((uint32_t)error << 8) | passive;
	}
	return ret;
}

static int st25r3916_wait_interrupt_unlocked(const struct device *dev,
					     uint32_t wanted,
					     uint32_t timeout_ms,
					     uint32_t *observed)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;
	uint32_t accumulated = 0U;
	uint32_t value;
	int ret;

	do {
		ret = st25r3916_read_interrupts_unlocked(dev, &value);
		if (ret < 0) {
			return ret;
		}
		accumulated |= value;
		if ((accumulated & wanted) != 0U) {
			*observed = accumulated;
			return 0;
		}
		if ((accumulated & ST25R3916_IRQ_NO_RESPONSE) != 0U) {
			*observed = accumulated;
			return -ETIMEDOUT;
		}
		k_busy_wait(100);
	} while (k_uptime_get() <= deadline);

	*observed = accumulated;
	return -ETIMEDOUT;
}

static int st25r3916_clear_interrupts_unlocked(const struct device *dev)
{
	uint32_t discard;

	return st25r3916_read_interrupts_unlocked(dev, &discard);
}

static int st25r3916_fifo_status_unlocked(const struct device *dev,
					  uint16_t *bytes, uint8_t *bits)
{
	uint8_t status[2];
	uint16_t value;
	int ret;

	ret = st25r3916_read_unlocked(
		dev, ST25R3916_REG_FIFO_STATUS_1, status, sizeof(status));
	if (ret == 0) {
		value = sys_get_be16(status);
		*bytes = (value >> 8) | ((value & 0x00C0U) << 2);
		*bits = (value >> 1) & 0x07U;
	}
	return ret;
}

static int st25r3916_fifo_read_unlocked(const struct device *dev,
					uint8_t *buffer, size_t capacity,
					size_t *actual, uint8_t *bits)
{
	uint16_t available;
	uint8_t trailing_bits;
	int ret;

	ret = st25r3916_fifo_status_unlocked(
		dev, &available, &trailing_bits);
	if (ret < 0) {
		return ret;
	}
	if (available == 0U) {
		return -ENOMSG;
	}
	if (available > capacity || available > ST25R3916_MAX_BURST) {
		return -EMSGSIZE;
	}
	ret = st25r3916_command_read_unlocked(
		dev, ST25R3916_OP_READ_FIFO, buffer, available);
	if (ret == 0) {
		*actual = available;
		*bits = trailing_bits;
	}
	return ret;
}

static int st25r3916_set_frame_timeout_unlocked(const struct device *dev,
						uint32_t timeout_ms)
{
	uint8_t timer_control;
	uint64_t step;
	uint64_t count;
	int ret;

	ret = st25r3916_read_unlocked(dev, ST25R3916_REG_TIMER_CONTROL,
				      &timer_control, sizeof(timer_control));
	if (ret < 0) {
		return ret;
	}
	/* NRT uses either 64/fc or 4096/fc, with fc = 13.56 MHz. */
	step = (timer_control & BIT(0)) != 0U ? 4096ULL * 1000000ULL :
		64ULL * 1000000ULL;
	count = ((uint64_t)timeout_ms * 1000ULL * 13560000ULL +
		 step - 1ULL) / step;
	count = CLAMP(count, 1ULL, 0xFFFFULL);
	return st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_NO_RESPONSE_1, (uint16_t)count);
}

static int st25r3916_write_tx_length_unlocked(const struct device *dev,
					      uint16_t bytes, uint8_t bits)
{
	const uint16_t value = ((bytes & 0x01FFU) << 3) | (bits & 0x07U);

	return st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_TX_BYTES_1, value);
}

static int st25r3916_transceive_unlocked(const struct device *dev,
					 const uint8_t *tx, size_t tx_length,
					 bool with_crc, uint8_t *rx,
					 size_t rx_capacity, size_t *rx_length,
					 uint32_t timeout_ms)
{
	uint32_t interrupts;
	uint8_t bits;
	int ret;

	ret = st25r3916_set_frame_timeout_unlocked(dev, timeout_ms);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0U,
		ST25R3916_AUX_NO_CRC_RX);
	ret = ret ?: st25r3916_clear_interrupts_unlocked(dev);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_CLEAR_FIFO, NULL, 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_OP_LOAD_FIFO, tx, tx_length);
	ret = ret ?: st25r3916_write_tx_length_unlocked(
		dev, tx_length, 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, with_crc ? ST25R3916_CMD_TRANSMIT_CRC :
			ST25R3916_CMD_TRANSMIT_NO_CRC, NULL, 0U);
	ret = ret ?: st25r3916_wait_interrupt_unlocked(
		dev, ST25R3916_IRQ_RX_END | ST25R3916_IRQ_COLLISION,
		timeout_ms, &interrupts);
	if (ret < 0) {
		return ret;
	}
	if ((interrupts & ST25R3916_IRQ_COLLISION) != 0U) {
		return -EAGAIN;
	}
	return st25r3916_fifo_read_unlocked(
		dev, rx, rx_capacity, rx_length, &bits);
}

static int st25r3916_configure_nfca_unlocked(const struct device *dev)
{
	static const uint8_t protection[] = {0x04U, 0x10U};
	uint32_t interrupts;
	uint8_t mask;
	uint8_t auxiliary;
	uint8_t regulator;
	int ret;

	ret = st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_STOP_ALL, NULL, 0U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL, 0U,
		ST25R3916_OP_CTRL_TX_ENABLE | ST25R3916_OP_CTRL_RX_ENABLE);
	if (ret < 0) {
		LOG_ERR("NFC-A stop/reset stage failed (%d)", ret);
		return ret;
	}
	k_msleep(2);

	ret = st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_SET_DEFAULT, NULL, 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_TEST_ACCESS, protection,
		sizeof(protection));
	/* 100 kHz I2C, 3.3 V supply, stronger digital output drive. */
	ret = ret ?: st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_1, 0x0084U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TX_DRIVER, 0x00U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_1, 0x07U, 0x07U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_RESISTIVE_AM, 0x80U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_2, BIT(5), 0U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_RESISTIVE_AM, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_FIELD_ON_THRESHOLD, 0x13U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_FIELD_OFF_THRESHOLD, 0x02U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_NFCIP1_TARGET, 0x50U, 0xF0U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TARGET_MODULATION, 0x5FU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_EMD_SUPPRESSION, 0x40U);
	ret = ret ?: st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_ANT_TUNE_1, 0x8282U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL, 0x03U, 0x03U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_CLEAR_FIFO, NULL, 0U);
	if (ret < 0) {
		LOG_ERR("NFC-A base configuration stage failed (%d)", ret);
		return ret;
	}

	/* Mask all but error IRQs while the oscillator starts. */
	static const uint8_t startup_masks[] = {0xFFU, 0xFFU, 0x00U, 0xFFU};
	ret = st25r3916_write_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, startup_masks,
		sizeof(startup_masks));
	ret = ret ?: st25r3916_clear_interrupts_unlocked(dev);
	ret = ret ?: st25r3916_read_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, &mask, sizeof(mask));
	if (ret == 0) {
		mask &= ~BIT(7);
		ret = st25r3916_write8_unlocked(
			dev, ST25R3916_REG_MASK_MAIN_IRQ, mask);
	}
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL,
		ST25R3916_OP_CTRL_ENABLE, 0U);
	if (ret == 0) {
		ret = st25r3916_wait_interrupt_unlocked(
			dev, ST25R3916_IRQ_OSC, 50U, &interrupts);
		if (ret == -ETIMEDOUT) {
			ret = st25r3916_read_unlocked(
				dev, ST25R3916_REG_AUX_DISPLAY,
				&auxiliary, sizeof(auxiliary));
			if (ret == 0 &&
			    (auxiliary & ST25R3916_AUX_OSC_OK) == 0U) {
				ret = -ETIMEDOUT;
			}
		}
	}
	if (ret < 0) {
		LOG_ERR("NFC-A oscillator stage failed (%d)", ret);
		return ret;
	}
	ret = st25r3916_read_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, &mask, sizeof(mask));
	if (ret == 0) {
		mask |= BIT(7);
		ret = st25r3916_write8_unlocked(
			dev, ST25R3916_REG_MASK_MAIN_IRQ, mask);
	}
	static const uint8_t unmask_all[] = {0U, 0U, 0U, 0U};
	ret = ret ?: st25r3916_write_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, unmask_all,
		sizeof(unmask_all));
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_ADJUST_REGULATORS, NULL, 0U);
	if (ret < 0) {
		LOG_ERR("NFC-A regulator command stage failed (%d)", ret);
		return ret;
	}
	k_msleep(5);
	ret = st25r3916_space_b_read_unlocked(
		dev, ST25R3916_REG_B_REGULATOR_DISPLAY, &regulator);
	if (ret < 0) {
		LOG_ERR("NFC-A regulator read stage failed (%d)", ret);
		return ret;
	}

	/* ISO/IEC 14443-A initiator, 106 kbit/s, board antenna tuning. */
	ret = st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MODE_DEFINITION, 0x09U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_BITRATE_DEFINITION, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_ISO14443A_SETTINGS, 0x00U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0U, BIT(2));
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_OVERSHOOT_1, 0x40U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_OVERSHOOT_2, 0x03U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_UNDERSHOOT_1, 0x40U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_UNDERSHOOT_2, 0x03U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_1, 0x47U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_2, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_1, 0x08U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_2, 0x2DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_3, 0xD8U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_4, 0x22U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_RESET_RX_GAIN, NULL, 0U);
	ret = ret ?: st25r3916_write_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, unmask_all,
		sizeof(unmask_all));
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_INITIAL_FIELD_ON, NULL, 0U);
	if (ret < 0) {
		LOG_ERR("NFC-A mode configuration stage failed (%d)", ret);
		return ret;
	}
	k_msleep(5);
	ret = st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL,
		ST25R3916_OP_CTRL_TX_ENABLE | ST25R3916_OP_CTRL_RX_ENABLE,
		0U);
	if (ret == 0) {
		LOG_INF("ST25R3916 NFC-A RF field ready regulator=0x%02x",
			regulator);
	}
	return ret;
}

static int st25r3916_nfca_request_unlocked(const struct device *dev,
					   uint16_t *atqa)
{
	uint32_t interrupts;
	uint8_t response[2];
	uint8_t bits;
	size_t actual;
	uint16_t fifo_bytes;
	int ret;

	ret = st25r3916_set_frame_timeout_unlocked(
		dev, ST25R3916_NFCA_REQA_TIMEOUT_MS);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_ISO14443A_SETTINGS,
		ST25R3916_ISOA_ANTICOLLISION);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION,
		ST25R3916_AUX_NO_CRC_RX, 0U);
	ret = ret ?: st25r3916_clear_interrupts_unlocked(dev);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_CLEAR_FIFO, NULL, 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_TRANSMIT_REQA, NULL, 0U);
	ret = ret ?: st25r3916_wait_interrupt_unlocked(
		dev, ST25R3916_IRQ_RX_END | ST25R3916_IRQ_RX_START |
			ST25R3916_IRQ_COLLISION,
		ST25R3916_NFCA_REQA_TIMEOUT_MS, &interrupts);
	if (ret == -ETIMEDOUT) {
		return -ENOMSG;
	}
	if (ret < 0) {
		return ret;
	}

	/* Some revisions expose RX-start before RX-end; FIFO length is the
	 * authoritative completion fallback used by the factory stack.
	 */
	if ((interrupts & ST25R3916_IRQ_RX_END) == 0U) {
		const int64_t deadline = k_uptime_get() +
			ST25R3916_NFCA_REQA_TIMEOUT_MS;
		do {
			ret = st25r3916_fifo_status_unlocked(
				dev, &fifo_bytes, &bits);
			if (ret < 0 || fifo_bytes >= sizeof(response)) {
				break;
			}
			k_busy_wait(100);
		} while (k_uptime_get() <= deadline);
		if (ret < 0) {
			return ret;
		}
	}

	ret = st25r3916_fifo_read_unlocked(
		dev, response, sizeof(response), &actual, &bits);
	if (ret == -ENOMSG) {
		return -ENOMSG;
	}
	if (ret < 0 || actual != sizeof(response)) {
		return ret < 0 ? ret : -EBADMSG;
	}
	*atqa = sys_get_le16(response);
	return 0;
}

static int st25r3916_nfca_anticollision_level_unlocked(
	const struct device *dev, uint8_t level, uint8_t response[5])
{
	const uint8_t select_code = 0x91U + level * 2U;
	const uint8_t frame[2] = {select_code, 0x20U};
	uint32_t interrupts;
	uint8_t bits;
	size_t actual;
	int ret;

	ret = st25r3916_set_frame_timeout_unlocked(
		dev, ST25R3916_NFCA_FRAME_TIMEOUT_MS);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_ISO14443A_SETTINGS,
		ST25R3916_ISOA_ANTICOLLISION);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0U,
		ST25R3916_AUX_NO_CRC_RX);
	ret = ret ?: st25r3916_clear_interrupts_unlocked(dev);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_CLEAR_FIFO, NULL, 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_OP_LOAD_FIFO, frame, sizeof(frame));
	ret = ret ?: st25r3916_write_tx_length_unlocked(
		dev, sizeof(frame), 0U);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_TRANSMIT_NO_CRC, NULL, 0U);
	ret = ret ?: st25r3916_wait_interrupt_unlocked(
		dev, ST25R3916_IRQ_RX_END | ST25R3916_IRQ_COLLISION,
		ST25R3916_NFCA_FRAME_TIMEOUT_MS, &interrupts);
	if (ret < 0) {
		return ret;
	}
	if ((interrupts & ST25R3916_IRQ_COLLISION) != 0U) {
		uint8_t collision;

		(void)st25r3916_read_unlocked(
			dev, ST25R3916_REG_COLLISION_DISPLAY,
			&collision, sizeof(collision));
		LOG_WRN("NFC-A collision at cascade level %u display=0x%02x",
			level, collision);
		return -EAGAIN;
	}
	ret = st25r3916_fifo_read_unlocked(
		dev, response, 5U, &actual, &bits);
	if (ret < 0 || actual != 5U ||
	    (response[0] ^ response[1] ^ response[2] ^ response[3]) !=
		response[4]) {
		return ret < 0 ? ret : -EBADMSG;
	}
	return 0;
}

static int st25r3916_nfca_scan_unlocked(const struct device *dev,
					struct papermono_nfca_tag *tag)
{
	uint8_t anticollision[5];
	uint8_t select_frame[7];
	uint8_t select_response[3];
	size_t response_length;
	int ret;

	memset(tag, 0, sizeof(*tag));
	ret = st25r3916_nfca_request_unlocked(dev, &tag->atqa);
	if (ret < 0) {
		return ret;
	}

	for (uint8_t level = 1U; level <= 3U; ++level) {
		ret = st25r3916_nfca_anticollision_level_unlocked(
			dev, level, anticollision);
		if (ret < 0) {
			return ret;
		}

		const size_t source_offset =
			anticollision[0] == ST25R3916_CASCADE_TAG ? 1U : 0U;
		const size_t copy_length = 4U - source_offset;
		if (tag->uid_length + copy_length > sizeof(tag->uid)) {
			return -EOVERFLOW;
		}
		memcpy(&tag->uid[tag->uid_length],
		       &anticollision[source_offset], copy_length);
		tag->uid_length += copy_length;

		select_frame[0] = 0x91U + level * 2U;
		select_frame[1] = 0x70U;
		memcpy(&select_frame[2], anticollision, sizeof(anticollision));
		ret = st25r3916_transceive_unlocked(
			dev, select_frame, sizeof(select_frame), true,
			select_response, sizeof(select_response), &response_length,
			ST25R3916_NFCA_FRAME_TIMEOUT_MS);
		if (ret < 0 || response_length != sizeof(select_response)) {
			return ret < 0 ? ret : -EBADMSG;
		}
		tag->sak = select_response[0];
		if ((tag->sak & ST25R3916_SAK_CASCADE) == 0U) {
			return 0;
		}
	}

	return -EBADMSG;
}

static int st25r3916_field_on_unlocked(const struct device *dev)
{
	int ret;

	ret = st25r3916_clear_interrupts_unlocked(dev);
	ret = ret ?: st25r3916_command_write_unlocked(
		dev, ST25R3916_CMD_INITIAL_FIELD_ON, NULL, 0U);
	if (ret < 0) {
		return ret;
	}
	k_msleep(5);
	return st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL,
		ST25R3916_OP_CTRL_TX_ENABLE | ST25R3916_OP_CTRL_RX_ENABLE,
		0U);
}

static int st25r3916_configure_nfcb_unlocked(const struct device *dev)
{
	int ret;

	/* ISO/IEC 14443-B initiator at 106 kbit/s. Values follow the
	 * M5Stack ST25R3916 reference configuration for this antenna.
	 */
	ret = st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MODE_DEFINITION, 0x14U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_BITRATE_DEFINITION, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_ISO14443B_SETTINGS, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_NFCIP1_TARGET, 0x5DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_STREAM_MODE, 0x38U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_1, 0x04U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_2, 0x3DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_3, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_4, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(dev, 0x0FU, 0x07U);
	ret = ret ?: st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_NO_RESPONSE_1, 0x100DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TIMER_CONTROL, 0x23U);
	ret = ret ?: st25r3916_write16_be_unlocked(
		dev, ST25R3916_REG_GPT_1, 0x0058U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_PPON2, 0x80U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, 0x85U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MASK_TIMER_IRQ, 0xA6U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MASK_ERROR_IRQ, 0x0FU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MASK_PASSIVE_IRQ, 0x7BU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TX_DRIVER, 0x70U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TARGET_MODULATION, 0x5FU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_FIELD_ON_THRESHOLD, 0x13U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_FIELD_OFF_THRESHOLD, 0x02U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_EMD_SUPPRESSION, 0xC4U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_SUBCARRIER_TIMER, 0x14U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_P2P_RX_CONFIG, 0x0CU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_1, 0x1BU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_2, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_SQUELCH_TIMER, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_FIELD_GUARD, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_AUX_MODULATION, 0x10U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_TX_TIMING, 0x7CU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_RESISTIVE_AM, 0x80U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_REGULATOR_CONTROL, 0xD0U);
	ret = ret ?: st25r3916_field_on_unlocked(dev);
	if (ret == 0) {
		LOG_INF("ST25R3916 NFC-B RF field ready");
	}
	return ret;
}

static int st25r3916_nfcb_scan_unlocked(const struct device *dev,
					struct papermono_nfcb_tag *tag)
{
	const uint8_t request[] = {0x05U, 0x00U, 0x00U};
	uint8_t response[14];
	size_t response_length;
	int ret;

	memset(tag, 0, sizeof(*tag));
	ret = st25r3916_transceive_unlocked(
		dev, request, sizeof(request), true, response, sizeof(response),
		&response_length, ST25R3916_NFCB_TIMEOUT_MS);
	if (ret == -ETIMEDOUT) {
		return -ENOMSG;
	}
	if (ret < 0) {
		return ret;
	}
	if (response_length < 12U || response[0] != 0x50U) {
		return -EBADMSG;
	}
	memcpy(tag->pupi, &response[1], sizeof(tag->pupi));
	memcpy(tag->application_data, &response[5], sizeof(tag->application_data));
	memcpy(tag->protocol_info, &response[9], sizeof(tag->protocol_info));
	return 0;
}

static int st25r3916_configure_nfcf_unlocked(const struct device *dev)
{
	static const uint8_t irq_masks[] = {0U, 0U, 0U, 0U};
	int ret;

	ret = st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MODE_DEFINITION, 0x1CU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_BITRATE_DEFINITION, 0x11U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL, 0U, 0x03U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_1, 0x07U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_1, 0x13U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_2, 0x3DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_3, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_4, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_1, 0x54U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_2, 0x00U);
	ret = ret ?: st25r3916_write_unlocked(
		dev, ST25R3916_REG_MASK_MAIN_IRQ, irq_masks,
		sizeof(irq_masks));
	ret = ret ?: st25r3916_field_on_unlocked(dev);
	if (ret == 0) {
		LOG_INF("ST25R3916 NFC-F RF field ready");
	}
	return ret;
}

static int st25r3916_nfcf_scan_unlocked(const struct device *dev,
					struct papermono_nfcf_tag *tag)
{
	/* SENSF_REQ: wildcard system code, no request data, one slot. */
	const uint8_t request[] = {0x00U, 0xFFU, 0xFFU, 0x00U, 0x00U};
	uint8_t response[20];
	size_t response_length;
	int ret;

	memset(tag, 0, sizeof(*tag));
	ret = st25r3916_transceive_unlocked(
		dev, request, sizeof(request), true, response, sizeof(response),
		&response_length, ST25R3916_NFCF_TIMEOUT_MS);
	if (ret == -ETIMEDOUT) {
		return -ENOMSG;
	}
	if (ret < 0) {
		return ret;
	}
	if (response_length < 18U || response[0] < 18U ||
	    response[1] != 0x01U) {
		return -EBADMSG;
	}
	memcpy(tag->idm, &response[2], sizeof(tag->idm));
	memcpy(tag->pmm, &response[10], sizeof(tag->pmm));
	return 0;
}

static uint16_t st25r3916_iso15693_crc(const uint8_t *buffer, size_t length)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0U; i < length; ++i) {
		crc ^= buffer[i];
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0x8408U : crc >> 1;
		}
	}
	return ~crc;
}

static int st25r3916_nfcv_encode_1of4(const uint8_t *frame,
				      size_t frame_length, uint8_t *encoded,
				      size_t capacity, size_t *encoded_length)
{
	static const uint8_t symbols[] = {0x02U, 0x08U, 0x20U, 0x80U};
	uint8_t with_crc[ST25R3916_MAX_BURST / 4U];
	size_t output = 0U;
	uint16_t crc;

	if (frame_length + 2U > sizeof(with_crc) ||
	    2U + (frame_length + 2U) * 4U > capacity) {
		return -EMSGSIZE;
	}
	memcpy(with_crc, frame, frame_length);
	crc = st25r3916_iso15693_crc(frame, frame_length);
	with_crc[frame_length] = crc & 0xFFU;
	with_crc[frame_length + 1U] = crc >> 8;

	encoded[output++] = 0x21U;
	for (size_t i = 0U; i < frame_length + 2U; ++i) {
		uint8_t value = with_crc[i];

		for (uint8_t pair = 0U; pair < 4U; ++pair) {
			encoded[output++] = symbols[value & 0x03U];
			value >>= 2;
		}
	}
	encoded[output++] = 0x04U;
	*encoded_length = output;
	return 0;
}

static int st25r3916_nfcv_decode(const uint8_t *encoded,
				 size_t encoded_length, uint8_t *frame,
				 size_t capacity, size_t *frame_length)
{
	uint8_t decoded[32] = {0};
	const size_t encoded_bits = encoded_length * 8U;
	size_t decoded_bits = 0U;
	uint16_t crc;
	uint16_t received_crc;

	if (encoded_length == 0U || (encoded[0] & 0x1FU) != 0x17U) {
		return -EBADMSG;
	}
	for (size_t bit = 5U; bit + 2U < encoded_bits; bit += 2U) {
		const uint8_t manchester =
			((encoded[bit / 8U] >> (bit % 8U)) & 1U) |
			(((encoded[(bit + 1U) / 8U] >>
			   ((bit + 1U) % 8U)) & 1U) << 1);
		bool eof = false;

		if ((decoded_bits % 8U) == 0U) {
			const size_t byte = bit / 8U;

			eof = byte + 1U < encoded_length &&
			      (encoded[byte] & 0xE0U) == 0xA0U &&
			      encoded[byte + 1U] == 0x03U;
		}
		if (eof) {
			break;
		}
		if (manchester != 1U && manchester != 2U) {
			return -EAGAIN;
		}
		if (decoded_bits >= sizeof(decoded) * 8U) {
			return -EMSGSIZE;
		}
		if (manchester == 2U) {
			decoded[decoded_bits / 8U] |= BIT(decoded_bits % 8U);
		}
		++decoded_bits;
	}
	if ((decoded_bits % 8U) != 0U || decoded_bits / 8U <= 2U) {
		return -EBADMSG;
	}
	const size_t decoded_length = decoded_bits / 8U;
	crc = st25r3916_iso15693_crc(decoded, decoded_length - 2U);
	received_crc = sys_get_le16(&decoded[decoded_length - 2U]);
	if (crc != received_crc) {
		return -EBADMSG;
	}
	if (decoded_length - 2U > capacity) {
		return -EMSGSIZE;
	}
	memcpy(frame, decoded, decoded_length - 2U);
	*frame_length = decoded_length - 2U;
	return 0;
}

static int st25r3916_configure_nfcv_unlocked(const struct device *dev)
{
	int ret;

	ret = st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_1, 0x13U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_2, 0x3DU);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_3, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_RX_CONFIG_4, 0x00U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_TX_DRIVER, 0x70U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_1, 0x07U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_IO_CONFIG_2, BIT(5), 0U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL, 0x03U, 0U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_STREAM_MODE, 0x38U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_AUX_DEFINITION, 0x02U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_EMD_SUPPRESSION, 0x40U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_SUBCARRIER_TIMER, 0x14U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_P2P_RX_CONFIG, 0x0CU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_1, 0x13U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_CORRELATOR_2, 0x01U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_SQUELCH_TIMER, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_FIELD_GUARD, 0x00U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_AUX_MODULATION, 0x10U);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_TX_TIMING, 0x7CU);
	ret = ret ?: st25r3916_space_b_write_unlocked(
		dev, ST25R3916_REG_B_RESISTIVE_AM, 0x80U);
	ret = ret ?: st25r3916_write8_unlocked(
		dev, ST25R3916_REG_MODE_DEFINITION, 0x70U);
	ret = ret ?: st25r3916_field_on_unlocked(dev);
	if (ret == 0) {
		LOG_INF("ST25R3916 NFC-V RF field ready");
	}
	return ret;
}

static int st25r3916_nfcv_scan_unlocked(const struct device *dev,
					struct papermono_nfcv_tag *tag)
{
	/* ISO/IEC 15693 inventory, high data rate, one slot, no mask. */
	const uint8_t request[] = {0x26U, 0x01U, 0x00U};
	uint8_t encoded[32];
	uint8_t raw_response[ST25R3916_MAX_BURST];
	uint8_t response[16];
	size_t encoded_length;
	size_t raw_length;
	size_t response_length;
	int ret;

	memset(tag, 0, sizeof(*tag));
	ret = st25r3916_nfcv_encode_1of4(
		request, sizeof(request), encoded, sizeof(encoded),
		&encoded_length);
	ret = ret ?: st25r3916_transceive_unlocked(
		dev, encoded, encoded_length, false, raw_response,
		sizeof(raw_response), &raw_length, ST25R3916_NFCV_TIMEOUT_MS);
	if (ret == -ETIMEDOUT) {
		return -ENOMSG;
	}
	if (ret < 0) {
		return ret;
	}
	ret = st25r3916_nfcv_decode(
		raw_response, raw_length, response, sizeof(response),
		&response_length);
	if (ret < 0 || response_length < 10U || response[0] != 0x00U) {
		return ret < 0 ? ret : -EBADMSG;
	}
	tag->dsfid = response[1];
	for (uint8_t i = 0U; i < sizeof(tag->uid); ++i) {
		tag->uid[i] = response[9U - i];
	}
	return 0;
}

static int st25r3916_configure_mode_unlocked(
	const struct device *dev, enum st25r3916_rf_mode mode)
{
	int ret;

	ret = st25r3916_configure_nfca_unlocked(dev);
	if (ret < 0 || mode == ST25R3916_RF_A) {
		return ret;
	}
	switch (mode) {
	case ST25R3916_RF_B:
		return st25r3916_configure_nfcb_unlocked(dev);
	case ST25R3916_RF_F:
		return st25r3916_configure_nfcf_unlocked(dev);
	case ST25R3916_RF_V:
		return st25r3916_configure_nfcv_unlocked(dev);
	default:
		return -EINVAL;
	}
}

static int st25r3916_read_unlocked(const struct device *dev, uint8_t reg,
				   uint8_t *data, size_t length)
{
	const struct papermono_st25r3916_config *config = dev->config;
	uint8_t command = ST25R3916_REGISTER_READ |
		(reg & ST25R3916_REGISTER_ADDRESS_MASK);
	int ret;

	ret = i2c_write_read_dt(&config->bus, &command, sizeof(command),
				data, length);
	if (ret == -EFAULT || ret == -ETIMEDOUT || ret == -EBUSY) {
		LOG_WRN("I2C register read 0x%02x failed (%d); recovering bus",
			reg, ret);
		if (i2c_recover_bus(config->bus.bus) == 0) {
			ret = i2c_write_read_dt(&config->bus, &command,
						sizeof(command), data, length);
		}
	}
	if (ret < 0) {
		LOG_ERR("I2C register read 0x%02x len=%u failed (%d)", reg,
			(unsigned int)length, ret);
	}
	return ret;
}

static int st25r3916_probe_unlocked(const struct device *dev)
{
	struct papermono_st25r3916_data *data = dev->data;
	uint8_t identity = 0U;
	int ret = -ENODEV;

	for (uint8_t attempt = 0U; attempt < ST25R3916_IDENTITY_RETRIES;
	     ++attempt) {
		ret = st25r3916_read_unlocked(
			dev, ST25R3916_REG_IDENTITY, &identity, sizeof(identity));
		if (ret == 0 && ((identity >> 3) & 0x1FU) ==
			ST25R3916_EXPECTED_TYPE && (identity & 0x07U) != 0U) {
			data->info.identity = identity;
			data->info.type = (identity >> 3) & 0x1FU;
			data->info.revision = identity & 0x07U;
			return 0;
		}
		k_msleep(ST25R3916_IDENTITY_RETRY_MS);
	}

	return ret < 0 ? ret : -ENODEV;
}

static int st25r3916_validate_thread_call(const struct device *dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	return device_is_ready(dev) ? 0 : -ENODEV;
}

int papermono_st25r3916_set_power(const struct device *dev, bool enable)
{
	const struct papermono_st25r3916_config *config;
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	config = dev->config;
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (enable == data->powered) {
		k_mutex_unlock(&data->lock);
		return 0;
	}

	if (!enable && data->powered) {
		(void)st25r3916_command_write_unlocked(
			dev, ST25R3916_CMD_STOP_ALL, NULL, 0U);
	}
	ret = gpio_pin_set_dt(&config->power, enable ? 1 : 0);
	if (ret < 0) {
		LOG_ERR("NFC power GPIO update to %u failed (%d)",
			enable ? 1U : 0U, ret);
	}
	if (ret == 0 && enable) {
		k_msleep(config->power_on_delay_ms);
		ret = st25r3916_probe_unlocked(dev);
		if (ret == 0) {
			data->powered = true;
			LOG_INF("ST25R3916 identity=0x%02x type=0x%02x revision=%u",
				data->info.identity, data->info.type,
				data->info.revision);
		} else {
			LOG_ERR("ST25R3916 probe after power-on failed (%d)", ret);
			(void)gpio_pin_set_dt(&config->power, 0);
		}
	} else if (ret == 0) {
		data->powered = false;
		data->rf_mode = ST25R3916_RF_NONE;
		data->info = (struct papermono_st25r3916_info){0};
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_get_info(const struct device *dev,
					 struct papermono_st25r3916_info *info)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (info == NULL) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->powered) {
		k_mutex_unlock(&data->lock);
		return -EACCES;
	}
	*info = data->info;
	k_mutex_unlock(&data->lock);
	return 0;
}

int papermono_st25r3916_read(const struct device *dev, uint8_t reg,
				     uint8_t *data_out, size_t length)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (data_out == NULL || length == 0U || length > ST25R3916_MAX_BURST) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = data->powered ? st25r3916_read_unlocked(
		dev, reg, data_out, length) : -EACCES;
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_write(const struct device *dev, uint8_t reg,
				      const uint8_t *data_out, size_t length)
{
	const struct papermono_st25r3916_config *config;
	struct papermono_st25r3916_data *data;
	uint8_t buffer[ST25R3916_MAX_BURST + 1U];
	int ret;

	if (data_out == NULL || length == 0U || length > ST25R3916_MAX_BURST) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	config = dev->config;
	data = dev->data;
	buffer[0] = reg & ST25R3916_REGISTER_ADDRESS_MASK;
	memcpy(&buffer[1], data_out, length);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = data->powered ? i2c_write_dt(
		&config->bus, buffer, length + 1U) : -EACCES;
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_direct_command(const struct device *dev,
					       uint8_t command)
{
	const struct papermono_st25r3916_config *config;
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	config = dev->config;
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = data->powered ? i2c_write_dt(
		&config->bus, &command, sizeof(command)) : -EACCES;
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_configure_nfca(const struct device *dev)
{
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->powered) {
		ret = -EACCES;
	} else if (data->rf_mode == ST25R3916_RF_A) {
		ret = 0;
	} else {
		ret = st25r3916_configure_mode_unlocked(
			dev, ST25R3916_RF_A);
		if (ret == 0) {
			data->rf_mode = ST25R3916_RF_A;
		}
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_nfca_scan(const struct device *dev,
				  struct papermono_nfca_tag *tag)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (tag == NULL) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = !data->powered ? -EACCES :
		data->rf_mode != ST25R3916_RF_A ? -EHOSTDOWN :
		st25r3916_nfca_scan_unlocked(dev, tag);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int st25r3916_configure_public(const struct device *dev,
				      enum st25r3916_rf_mode mode)
{
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->powered) {
		ret = -EACCES;
	} else if (data->rf_mode == mode) {
		ret = 0;
	} else {
		ret = st25r3916_configure_mode_unlocked(dev, mode);
		if (ret == 0) {
			data->rf_mode = mode;
		}
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_configure_nfcb(const struct device *dev)
{
	return st25r3916_configure_public(dev, ST25R3916_RF_B);
}

int papermono_st25r3916_nfcb_scan(const struct device *dev,
				  struct papermono_nfcb_tag *tag)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (tag == NULL) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = !data->powered ? -EACCES :
		data->rf_mode != ST25R3916_RF_B ? -EHOSTDOWN :
		st25r3916_nfcb_scan_unlocked(dev, tag);
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_configure_nfcf(const struct device *dev)
{
	return st25r3916_configure_public(dev, ST25R3916_RF_F);
}

int papermono_st25r3916_nfcf_scan(const struct device *dev,
				  struct papermono_nfcf_tag *tag)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (tag == NULL) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = !data->powered ? -EACCES :
		data->rf_mode != ST25R3916_RF_F ? -EHOSTDOWN :
		st25r3916_nfcf_scan_unlocked(dev, tag);
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_configure_nfcv(const struct device *dev)
{
	return st25r3916_configure_public(dev, ST25R3916_RF_V);
}

int papermono_st25r3916_nfcv_scan(const struct device *dev,
				  struct papermono_nfcv_tag *tag)
{
	struct papermono_st25r3916_data *data;
	int ret;

	if (tag == NULL) {
		return -EINVAL;
	}
	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = !data->powered ? -EACCES :
		data->rf_mode != ST25R3916_RF_V ? -EHOSTDOWN :
		st25r3916_nfcv_scan_unlocked(dev, tag);
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_disable_field(const struct device *dev)
{
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = !data->powered ? -EACCES :
		st25r3916_command_write_unlocked(
			dev, ST25R3916_CMD_STOP_ALL, NULL, 0U);
	ret = ret ?: st25r3916_update8_unlocked(
		dev, ST25R3916_REG_OPERATION_CONTROL, 0U,
		ST25R3916_OP_CTRL_TX_ENABLE | ST25R3916_OP_CTRL_RX_ENABLE);
	if (ret == 0) {
		data->rf_mode = ST25R3916_RF_NONE;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_st25r3916_set_irq_callback(
	const struct device *dev, papermono_st25r3916_irq_callback_t callback,
	void *user_data)
{
	struct papermono_st25r3916_data *data;
	int ret;

	ret = st25r3916_validate_thread_call(dev);
	if (ret < 0) {
		return ret;
	}
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->irq_callback = callback;
	data->irq_user_data = user_data;
	k_mutex_unlock(&data->lock);
	return 0;
}

static void st25r3916_irq_handler(const struct device *port,
				  struct gpio_callback *callback,
				  gpio_port_pins_t pins)
{
	struct papermono_st25r3916_data *data = CONTAINER_OF(
		callback, struct papermono_st25r3916_data, irq_gpio_callback);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	if (data->powered && data->irq_callback != NULL) {
		data->irq_callback(data->dev, data->irq_user_data);
	}
}

static int papermono_st25r3916_init(const struct device *dev)
{
	const struct papermono_st25r3916_config *config = dev->config;
	struct papermono_st25r3916_data *data = dev->data;
	int ret;

	if (!i2c_is_ready_dt(&config->bus) ||
	    !gpio_is_ready_dt(&config->power) ||
	    !gpio_is_ready_dt(&config->irq)) {
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	data->dev = dev;
	ret = gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
	ret = ret ?: gpio_pin_configure_dt(&config->irq, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}
	gpio_init_callback(&data->irq_gpio_callback, st25r3916_irq_handler,
			   BIT(config->irq.pin));
	ret = gpio_add_callback(config->irq.port, &data->irq_gpio_callback);
	ret = ret ?: gpio_pin_interrupt_configure_dt(
		&config->irq, GPIO_INT_EDGE_TO_ACTIVE);
	return ret;
}

#define PAPERMONO_ST25R3916_DEFINE(inst)                                      \
	static const struct papermono_st25r3916_config                          \
		papermono_st25r3916_config_##inst = {                             \
			.bus = I2C_DT_SPEC_INST_GET(inst),                          \
			.power = GPIO_DT_SPEC_INST_GET(inst, power_gpios),          \
			.irq = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),              \
			.power_on_delay_ms = DT_INST_PROP(inst, power_on_delay_ms),  \
	};                                                                         \
	static struct papermono_st25r3916_data                                   \
		papermono_st25r3916_data_##inst;                                   \
	DEVICE_DT_INST_DEFINE(inst, papermono_st25r3916_init, NULL,               \
			      &papermono_st25r3916_data_##inst,                      \
			      &papermono_st25r3916_config_##inst, POST_KERNEL,       \
			      CONFIG_PAPERMONO_BOARD_FUNCTION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAPERMONO_ST25R3916_DEFINE)
