/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_TOUCHPAD_H_
#define PAPERMONO_TOUCHPAD_H_

#include <stdbool.h>
#include <stdint.h>

struct papermono_touchpad {
	int16_t start_x, start_y;
	int16_t last_x, last_y;
	int16_t scroll_remainder;
	bool moved;
	bool scrolling;
};

void papermono_touchpad_begin(struct papermono_touchpad *pad,
	int16_t x, int16_t y, bool scrolling);
/* Returns actual relative motion while held. Crossing the scroll boundary
 * after contact does not change the gesture's original mode.
 */
bool papermono_touchpad_update(struct papermono_touchpad *pad,
	int16_t x, int16_t y, int16_t *dx, int16_t *dy, int16_t *wheel);

#endif
