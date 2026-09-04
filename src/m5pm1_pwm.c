/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT m5stack_m5pm1_pwm

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <papermono/m5pm1.h>

#define M5PM1_PWM_CLOCK_HZ 1000000ULL

struct m5pm1_pwm_config {
	const struct device *parent;
};

static int m5pm1_pwm_set_cycles(const struct device *dev, uint32_t channel,
				 uint32_t period_cycles,
				 uint32_t pulse_cycles, pwm_flags_t flags)
{
	const struct m5pm1_pwm_config *config = dev->config;
	uint64_t scaled;
	uint32_t frequency;
	uint16_t duty;

	if (channel >= M5PM1_PWM_COUNT || period_cycles == 0U ||
	    pulse_cycles > period_cycles ||
	    (flags & ~PWM_POLARITY_MASK) != 0U) {
		return -EINVAL;
	}

	frequency = DIV_ROUND_CLOSEST(M5PM1_PWM_CLOCK_HZ, period_cycles);
	if (frequency == 0U || frequency > UINT16_MAX) {
		return -EINVAL;
	}
	scaled = (uint64_t)pulse_cycles * 0x0FFFU;
	duty = (uint16_t)DIV_ROUND_CLOSEST(scaled, period_cycles);

	return m5pm1_pwm_set(config->parent, channel, (uint16_t)frequency,
			      duty, (flags & PWM_POLARITY_INVERTED) != 0U,
			      pulse_cycles != 0U);
}

static int m5pm1_pwm_get_cycles_per_sec(const struct device *dev,
					uint32_t channel, uint64_t *cycles)
{
	ARG_UNUSED(dev);
	if (channel >= M5PM1_PWM_COUNT) {
		return -EINVAL;
	}
	*cycles = M5PM1_PWM_CLOCK_HZ;
	return 0;
}

static DEVICE_API(pwm, m5pm1_pwm_api) = {
	.set_cycles = m5pm1_pwm_set_cycles,
	.get_cycles_per_sec = m5pm1_pwm_get_cycles_per_sec,
};

static int m5pm1_pwm_init(const struct device *dev)
{
	const struct m5pm1_pwm_config *config = dev->config;

	return device_is_ready(config->parent) ? 0 : -ENODEV;
}

#define M5PM1_PWM_DEFINE(inst)                                             \
	static const struct m5pm1_pwm_config m5pm1_pwm_config_##inst = {     \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(inst)),                   \
	};                                                                       \
	DEVICE_DT_INST_DEFINE(inst, m5pm1_pwm_init, NULL, NULL,                   \
			      &m5pm1_pwm_config_##inst, POST_KERNEL,            \
			      CONFIG_PAPERMONO_MFD_CHILD_INIT_PRIORITY,          \
			      &m5pm1_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(M5PM1_PWM_DEFINE)
