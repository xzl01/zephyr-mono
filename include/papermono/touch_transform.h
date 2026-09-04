/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_TOUCH_TRANSFORM_H_
#define PAPERMONO_TOUCH_TRANSFORM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_touch_point {
	uint16_t x;
	uint16_t y;
};

/**
 * Map the Paper Mono FT6336 portrait frame to the portrait UI frame.
 *
 * The controller and the 480x800 application use the same origin and axes.
 * This function deliberately rejects an out-of-range controller frame instead
 * of clamping it to an edge, where it could activate a real UI control.
 */
int papermono_touch_map_portrait(uint16_t raw_x, uint16_t raw_y,
	uint16_t width, uint16_t height, struct papermono_touch_point *point);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_TOUCH_TRANSFORM_H_ */
