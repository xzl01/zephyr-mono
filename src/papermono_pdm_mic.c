/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT m5stack_papermono_pdm_mic

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/i2s_hal.h>

#include <papermono/pdm_mic.h>

LOG_MODULE_REGISTER(papermono_pdm_mic, CONFIG_PAPERMONO_PDM_MIC_LOG_LEVEL);

#define PAPERMONO_PDM_CLOCK_RATIO 64U
#define PAPERMONO_PDM_MCLK_RATIO  128U
#define PAPERMONO_PDM_BLOCK_SIZE  512U
#define PAPERMONO_PDM_BLOCK_COUNT 4U

K_MEM_SLAB_DEFINE_STATIC(papermono_pdm_rx_slab, PAPERMONO_PDM_BLOCK_SIZE,
			 PAPERMONO_PDM_BLOCK_COUNT, 4);

struct papermono_pdm_mic_config {
	const struct device *i2s;
	struct gpio_dt_spec power;
	uintptr_t regs;
	uint32_t sample_rate_hz;
	uint32_t power_on_delay_ms;
};

struct papermono_pdm_mic_data {
	struct k_mutex lock;
	bool configured;
	bool running;
};

static void papermono_pdm_mic_set_mode(
	const struct papermono_pdm_mic_config *config)
{
	i2s_hal_context_t hal = {
		.dev = (i2s_dev_t *)config->regs,
	};
	i2s_hal_slot_config_t slot = {
		.data_bit_width = 16U,
		.slot_bit_width = 16U,
		.slot_mode = I2S_SLOT_MODE_MONO,
		.pdm_rx = {
			.slot_mask = I2S_PDM_SLOT_RIGHT,
			.data_fmt = I2S_PDM_DATA_FMT_PCM,
		},
	};
	i2s_hal_clock_info_t clock = {
		.sclk = 160000000U,
		.mclk = config->sample_rate_hz * PAPERMONO_PDM_MCLK_RATIO,
		.bclk = config->sample_rate_hz * PAPERMONO_PDM_CLOCK_RATIO,
		.bclk_div = PAPERMONO_PDM_MCLK_RATIO /
			    PAPERMONO_PDM_CLOCK_RATIO,
	};

	i2s_hal_pdm_set_rx_slot(&hal, false, &slot);
	i2s_hal_set_rx_clock(&hal, &clock, I2S_CLK_SRC_PLL_160M, NULL);
	i2s_ll_rx_set_pdm_dsr(hal.dev, I2S_PDM_DSR_16S);
	i2s_hal_pdm_enable_rx_channel(&hal, true);
	i2s_ll_rx_update(hal.dev);
}

static int papermono_pdm_mic_configure_locked(
	const struct device *dev)
{
	const struct papermono_pdm_mic_config *config = dev->config;
	struct papermono_pdm_mic_data *data = dev->data;
	struct i2s_config i2s_config = {
		.word_size = 16U,
		.channels = 2U,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		/* With neither TARGET bit set, Zephyr drives both clocks. */
		.options = 0U,
		.frame_clk_freq = config->sample_rate_hz,
		.mem_slab = &papermono_pdm_rx_slab,
		.block_size = PAPERMONO_PDM_BLOCK_SIZE,
		.timeout = 250,
	};
	int ret;

	if (data->configured) {
		return 0;
	}

	ret = i2s_configure(config->i2s, I2S_DIR_RX, &i2s_config);
	if (ret < 0) {
		LOG_ERR("I2S RX configure failed (%d)", ret);
		return ret;
	}

	papermono_pdm_mic_set_mode(config);
	data->configured = true;
	return 0;
}

int papermono_pdm_mic_start(const struct device *dev)
{
	const struct papermono_pdm_mic_config *config;
	struct papermono_pdm_mic_data *data;
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	config = dev->config;
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->running) {
		ret = 0;
		goto out;
	}

	/* Match the official board sequence and clear any stale PDM state. */
	ret = gpio_pin_set_dt(&config->power, 0);
	if (ret < 0) {
		LOG_ERR("microphone power-off GPIO failed (%d)", ret);
		goto out;
	}
	k_msleep(config->power_on_delay_ms);
	ret = gpio_pin_set_dt(&config->power, 1);
	if (ret < 0) {
		LOG_ERR("microphone power-on GPIO failed (%d)", ret);
		goto out;
	}
	k_msleep(config->power_on_delay_ms);

	ret = papermono_pdm_mic_configure_locked(dev);
	if (ret < 0) {
		goto power_off;
	}

	ret = i2s_trigger(config->i2s, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("I2S RX start failed (%d)", ret);
		goto power_off;
	}
	data->running = true;
	LOG_INF("PDM microphone started: %u Hz, GPIO45 clock, GPIO46 data",
		config->sample_rate_hz);
	goto out;

power_off:
	(void)gpio_pin_set_dt(&config->power, 0);
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_pdm_mic_read(const struct device *dev, int16_t *samples,
			   size_t capacity, size_t *count)
{
	const struct papermono_pdm_mic_config *config;
	struct papermono_pdm_mic_data *data;
	void *block = NULL;
	size_t block_size = 0U;
	size_t sample_count;
	int ret;

	if (dev == NULL || samples == NULL || capacity == 0U || count == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	config = dev->config;
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->running) {
		ret = -EACCES;
		goto out;
	}

	ret = i2s_read(config->i2s, &block, &block_size);
	if (ret < 0) {
		goto out;
	}

	sample_count = MIN(capacity, block_size / sizeof(int16_t));
	memcpy(samples, block, sample_count * sizeof(int16_t));
	*count = sample_count;
	k_mem_slab_free(&papermono_pdm_rx_slab, block);

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int papermono_pdm_mic_stop(const struct device *dev)
{
	const struct papermono_pdm_mic_config *config;
	struct papermono_pdm_mic_data *data;
	struct i2s_config unconfigure = {0};
	int power_ret;
	int ret = 0;

	if (dev == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	config = dev->config;
	data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->running) {
		ret = i2s_trigger(config->i2s, I2S_DIR_RX,
				  I2S_TRIGGER_DROP);
		data->running = false;
	}
	if (data->configured) {
		int config_ret = i2s_configure(config->i2s, I2S_DIR_RX,
					       &unconfigure);

		if (ret == 0) {
			ret = config_ret;
		}
		data->configured = false;
	}
	power_ret = gpio_pin_set_dt(&config->power, 0);
	if (ret == 0) {
		ret = power_ret;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int papermono_pdm_mic_init(const struct device *dev)
{
	const struct papermono_pdm_mic_config *config = dev->config;
	struct papermono_pdm_mic_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->i2s) ||
	    !gpio_is_ready_dt(&config->power)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	ret = gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("PaperMono PDM microphone transport ready");
	return 0;
}

#define PAPERMONO_PDM_MIC_DEFINE(inst)                                        \
	static struct papermono_pdm_mic_data papermono_pdm_mic_data_##inst;    \
	static const struct papermono_pdm_mic_config                            \
		papermono_pdm_mic_config_##inst = {                              \
			.i2s = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i2s)),           \
			.power = GPIO_DT_SPEC_INST_GET(inst, power_gpios),          \
			.regs = DT_REG_ADDR(DT_INST_PHANDLE(inst, i2s)),             \
			.sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),        \
			.power_on_delay_ms =                                        \
				DT_INST_PROP(inst, power_on_delay_ms),               \
		};                                                                   \
	DEVICE_DT_INST_DEFINE(inst, papermono_pdm_mic_init, NULL,                \
			      &papermono_pdm_mic_data_##inst,                     \
			      &papermono_pdm_mic_config_##inst, POST_KERNEL,       \
			      CONFIG_PAPERMONO_BOARD_FUNCTION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAPERMONO_PDM_MIC_DEFINE)
