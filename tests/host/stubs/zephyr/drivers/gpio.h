#ifndef ZEPHYR_DRIVERS_GPIO_H_
#define ZEPHYR_DRIVERS_GPIO_H_

#include <stdbool.h>
#include <zephyr/device.h>

#define GPIO_INPUT 0x01
#define GPIO_PULL_UP 0x02
#define GPIO_DT_SPEC_GET(node, property) { 0 }

struct gpio_dt_spec {
	const struct device *port;
	unsigned int pin;
	unsigned int dt_flags;
};

bool gpio_is_ready_dt(const struct gpio_dt_spec *spec);
int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags);
int gpio_pin_get_dt(const struct gpio_dt_spec *spec);

#endif
