/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/drivers/sensor.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_SENSOR_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_SENSOR_H_

#include <zephyr/device.h>

enum sensor_channel {
	SENSOR_CHAN_ACCEL_XYZ,
	SENSOR_CHAN_GYRO_XYZ,
	SENSOR_CHAN_ALL,
};

struct sensor_value {
	int32_t val1;
	int32_t val2;
};

int sensor_sample_fetch(const struct device *dev);
int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
	struct sensor_value *val);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_SENSOR_H_ */
