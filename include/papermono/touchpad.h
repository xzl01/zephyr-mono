/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_TOUCHPAD_H_
#define PAPERMONO_TOUCHPAD_H_

#include <stdbool.h>
#include <stdint.h>

struct papermono_touchpad {
	int16_t start_x, start_y;
	int16_t last_x, last_y;
	int16_t scroll_remainder;
	int16_t remainder_x, remainder_y;
	uint32_t last_motion_ms;
	bool moved;
	bool tracking;
	bool scrolling;
};

void papermono_touchpad_begin(struct papermono_touchpad *pad,
	int16_t x, int16_t y, uint32_t now_ms, bool scrolling);
/* Returns speed-adjusted relative motion while held, preserving fractional
 * counts. moved records the click slop independently from pointer tracking.
 * Crossing the scroll boundary cannot change the contact's original mode.
 */
bool papermono_touchpad_update(struct papermono_touchpad *pad,
	int16_t x, int16_t y, uint32_t now_ms,
	int16_t *dx, int16_t *dy, int16_t *wheel);

#endif
