/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/drivers/display.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_DISPLAY_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_DISPLAY_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

enum display_pixel_format {
	PIXEL_FORMAT_RGB_888,
	PIXEL_FORMAT_MONO01,
	PIXEL_FORMAT_MONO10,
};

enum display_screen_info {
	SCREEN_INFO_MONO_VTILED,
	SCREEN_INFO_MONO_MSB_FIRST,
	SCREEN_INFO_EPD,
	SCREEN_INFO_DOUBLE_BUFFER,
};

struct display_buffer_descriptor {
	uint32_t buf_size;
	uint16_t width;
	uint16_t height;
	uint16_t pitch;
};

struct display_capabilities {
	uint16_t x_resolution;
	uint16_t y_resolution;
	uint8_t supported_pixel_formats;
	uint8_t screen_info;
	uint8_t current_pixel_format;
	uint8_t current_orientation;
};

int display_write(const struct device *dev, uint16_t x, uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf);
int display_set_brightness(const struct device *dev, uint8_t brightness);
int display_blanking_on(const struct device *dev);
int display_blanking_off(const struct device *dev);
int display_get_capabilities(const struct device *dev,
	struct display_capabilities *caps);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DRIVERS_DISPLAY_H_ */
