/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stddef.h>

#include <papermono/touch_transform.h>

int papermono_touch_map_portrait(uint16_t raw_x, uint16_t raw_y,
	uint16_t width, uint16_t height, struct papermono_touch_point *point)
{
	if (point == NULL || width == 0U || height == 0U) {
		return -EINVAL;
	}
	if (raw_x >= width || raw_y >= height) {
		return -ERANGE;
	}

	point->x = raw_x;
	point->y = raw_y;
	return 0;
}
