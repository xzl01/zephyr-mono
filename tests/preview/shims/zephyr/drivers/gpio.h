/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/drivers/gpio.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_GPIO_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_GPIO_H_

#include <stdbool.h>

#include <zephyr/device.h>

#define GPIO_INPUT 0x01
#define GPIO_OUTPUT 0x02
#define GPIO_PULL_UP 0x04
#define GPIO_PULL_DOWN 0x08
#define GPIO_ACTIVE_LOW 0x10
/* The port pointer must be a compile-time constant because these specs are
 * used as static initialisers, and it must be non-NULL so gpio_is_ready_dt()
 * is true and the drivers take their "hardware present" paths.
 */
#define PAPERMONO_PREVIEW_GPIO_PORT(node) \
	(&papermono_preview_device_slots[(node) % PAPERMONO_PREVIEW_DEVICE_SLOTS])

#define GPIO_DT_SPEC_GET(node, property)                                   \
	{                                                                  \
		.port = PAPERMONO_PREVIEW_GPIO_PORT(node), .pin = 0,        \
		.dt_flags = 0                                              \
	}
#define GPIO_DT_SPEC_GET_OR(node, property, fallback) \
	GPIO_DT_SPEC_GET(node, property)
#define GPIO_DT_SPEC_INST_GET(inst, property) GPIO_DT_SPEC_GET(inst, property)
#define GPIO_DT_SPEC_GET_BY_IDX(node, property, idx) \
	GPIO_DT_SPEC_GET(node, property)

struct gpio_dt_spec {
	const struct device *port;
	unsigned int pin;
	unsigned int dt_flags;
};

struct gpio_callback {
	int unused;
};

static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	return spec != NULL && spec->port != NULL;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags);
int gpio_pin_get_dt(const struct gpio_dt_spec *spec);
int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value);
int gpio_pin_toggle_dt(const struct gpio_dt_spec *spec);
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *spec, int flags);
void gpio_init_callback(struct gpio_callback *callback, void *handler,
	unsigned int pin_mask);
int gpio_add_callback(const struct device *port, struct gpio_callback *callback);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_GPIO_H_ */
