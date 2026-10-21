/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_M5PM1_H_
#define PAPERMONO_M5PM1_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M5PM1_GPIO_COUNT 5U
#define M5PM1_PWM_COUNT  2U

#define M5PM1_POWER_CHARGE_ENABLE BIT(0)
#define M5PM1_POWER_DCDC_ENABLE   BIT(1)
#define M5PM1_POWER_LDO_ENABLE    BIT(2)
#define M5PM1_POWER_BOOST_ENABLE  BIT(3)
#define M5PM1_POWER_LED_LEVEL     BIT(4)
#define M5PM1_POWER_VALID_MASK    BIT_MASK(5)

#define M5PM1_HOLD_GPIO(pin) BIT(pin)
#define M5PM1_HOLD_LDO       BIT(5)
#define M5PM1_HOLD_BOOST     BIT(6)
#define M5PM1_HOLD_VALID_MASK (BIT_MASK(M5PM1_GPIO_COUNT) | \
				M5PM1_HOLD_LDO | M5PM1_HOLD_BOOST)

#define M5PM1_RTC_RAM_SIZE 32U

enum m5pm1_wake_edge {
	M5PM1_WAKE_FALLING,
	M5PM1_WAKE_RISING,
};

struct m5pm1_info {
	uint8_t id;
	uint8_t model;
	uint8_t hardware_revision;
	uint8_t software_revision;
};

struct m5pm1_irq_status {
	uint8_t wake_source;
	uint8_t gpio;
	uint8_t system;
	uint8_t button;
};

enum m5pm1_voltage {
	M5PM1_VOLTAGE_REFERENCE,
	M5PM1_VOLTAGE_BATTERY,
	M5PM1_VOLTAGE_INPUT,
	M5PM1_VOLTAGE_5V_OUTPUT,
};

enum m5pm1_system_command {
	M5PM1_SYSTEM_SHUTDOWN = 1,
	M5PM1_SYSTEM_REBOOT = 2,
	M5PM1_SYSTEM_DOWNLOAD = 3,
};

int m5pm1_get_info(const struct device *dev, struct m5pm1_info *info);
int m5pm1_reg_read(const struct device *dev, uint8_t reg, void *buffer,
		   size_t length);
int m5pm1_reg_write(const struct device *dev, uint8_t reg,
		    const void *buffer, size_t length);
int m5pm1_read_voltage(const struct device *dev, enum m5pm1_voltage source,
		       uint16_t *millivolts);
int m5pm1_read_adc(const struct device *dev, uint8_t channel,
		   uint16_t *sample);
int m5pm1_get_power_source(const struct device *dev, uint8_t *source);
int m5pm1_update_power(const struct device *dev, uint8_t mask, uint8_t value);
int m5pm1_set_power_hold(const struct device *dev, uint8_t mask,
			 bool enable);
int m5pm1_get_power_hold(const struct device *dev, uint8_t *mask);
int m5pm1_configure_wake_gpio(const struct device *dev, uint8_t pin,
			      enum m5pm1_wake_edge edge, bool enable);
int m5pm1_get_boot_wake_source(const struct device *dev, uint8_t *source);
int m5pm1_get_boot_wake_gpio_mask(const struct device *dev, uint8_t *mask);
int m5pm1_read_wake_source(const struct device *dev, uint8_t *source,
			   bool clear);
int m5pm1_set_i2c_sleep_timeout(const struct device *dev, uint8_t seconds);
int m5pm1_get_i2c_sleep_timeout(const struct device *dev, uint8_t *seconds);
int m5pm1_send_wake_signal(const struct device *dev);
int m5pm1_set_battery_lvp(const struct device *dev, uint16_t millivolts);
int m5pm1_rtc_ram_read(const struct device *dev, uint8_t offset,
			void *buffer, size_t length);
int m5pm1_rtc_ram_write(const struct device *dev, uint8_t offset,
			 const void *buffer, size_t length, bool verify);
int m5pm1_set_status_led(const struct device *dev, bool enabled,
			 bool open_drain);
int m5pm1_set_watchdog(const struct device *dev, uint8_t timeout_seconds);
int m5pm1_feed_watchdog(const struct device *dev);
int m5pm1_fetch_irq_status(const struct device *dev,
			   struct m5pm1_irq_status *status);
int m5pm1_clear_irq_status(const struct device *dev);
/* Unmask click/wake IRQs and disable single-click reset; retain long/double
 * press and download recovery policy. Used for awake input and deep-sleep wake.
 */
int m5pm1_prepare_button_wake(const struct device *dev);
int m5pm1_pwm_set(const struct device *dev, uint8_t channel,
		  uint16_t frequency_hz, uint16_t duty_12bit,
		  bool inverted, bool enable);
int m5pm1_system_command(const struct device *dev,
			 enum m5pm1_system_command command);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_M5PM1_H_ */
