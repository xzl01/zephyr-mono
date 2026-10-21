/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_M5IOE1_H_
#define PAPERMONO_M5IOE1_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M5IOE1_GPIO_COUNT 14U
#define M5IOE1_PWM_COUNT  4U

struct m5ioe1_info {
	uint16_t uid;
	uint8_t revision;
	uint16_t reference_mv;
};

enum m5ioe1_i2c_wake_edge {
	M5IOE1_I2C_WAKE_FALLING,
	M5IOE1_I2C_WAKE_RISING,
};

int m5ioe1_get_info(const struct device *dev, struct m5ioe1_info *info);
int m5ioe1_reg_read(const struct device *dev, uint8_t reg, void *buffer,
		    size_t length);
int m5ioe1_reg_write(const struct device *dev, uint8_t reg,
		     const void *buffer, size_t length);
int m5ioe1_read_adc(const struct device *dev, uint8_t channel,
		    uint16_t *sample);
int m5ioe1_read_temperature(const struct device *dev, uint16_t *sample);
int m5ioe1_pwm_set(const struct device *dev, uint8_t channel,
		   uint16_t frequency_hz, uint16_t duty_12bit,
		   bool inverted, bool enable);
int m5ioe1_set_i2c_sleep_timeout(const struct device *dev, uint8_t seconds);
int m5ioe1_get_i2c_sleep_timeout(const struct device *dev, uint8_t *seconds);
int m5ioe1_set_i2c_wake_edge(const struct device *dev,
			      enum m5ioe1_i2c_wake_edge edge);
int m5ioe1_get_i2c_wake_edge(const struct device *dev,
			      enum m5ioe1_i2c_wake_edge *edge);
int m5ioe1_send_wake_signal(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_M5IOE1_H_ */
