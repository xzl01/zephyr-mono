/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>
#include <papermono/touchpad.h>

void papermono_touchpad_begin(struct papermono_touchpad *pad,
	int16_t x, int16_t y, bool scrolling)
{
	*pad = (struct papermono_touchpad) {
		.start_x = x, .start_y = y, .last_x = x, .last_y = y,
		.scrolling = scrolling,
	};
}

bool papermono_touchpad_update(struct papermono_touchpad *pad,
	int16_t x, int16_t y, int16_t *dx, int16_t *dy, int16_t *wheel)
{
	*dx = *dy = *wheel = 0;
	if (!pad->moved) {
		/* Suppress finger-roll jitter around clicks, then preserve the full
		 * displacement when movement begins instead of losing the threshold.
		 */
		if (abs(x - pad->start_x) <= 6 && abs(y - pad->start_y) <= 6) {
			return false;
		}
		pad->moved = true;
	}
	if (pad->scrolling) {
		pad->scroll_remainder += pad->last_y - y;
		*wheel = pad->scroll_remainder / 16;
		pad->scroll_remainder %= 16;
	} else {
		*dx = x - pad->last_x;
		*dy = y - pad->last_y;
	}
	pad->last_x = x;
	pad->last_y = y;
	return *dx != 0 || *dy != 0 || *wheel != 0;
}
