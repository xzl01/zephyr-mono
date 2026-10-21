/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/input/input.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_INPUT_INPUT_H_
#define PAPERMONO_PREVIEW_ZEPHYR_INPUT_INPUT_H_

#include <stdint.h>

#include <zephyr/device.h>

enum input_event_type {
	INPUT_EV_KEY,
	INPUT_EV_REL,
	INPUT_EV_ABS,
	INPUT_EV_SYN,
};

struct input_event {
	const struct device *dev;
	uint32_t code;
	int32_t value;
	enum input_event_type type;
};

typedef void (*input_callback_t)(struct input_event *event, void *user_data);

int input_report_callback_register(const struct device *dev,
	input_callback_t callback, void *user_data);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_INPUT_INPUT_H_ */
