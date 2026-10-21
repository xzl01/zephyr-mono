/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <papermono/touchpad.h>

int main(void)
{
	struct papermono_touchpad pad;
	int16_t x, y, wheel;

	papermono_touchpad_begin(&pad, 200, 350, false);
	assert(!papermono_touchpad_update(&pad, 203, 354, &x, &y, &wheel));
	assert(!pad.moved); /* Click jitter remains a click. */
	assert(papermono_touchpad_update(&pad, 210, 358, &x, &y, &wheel));
	assert(pad.moved && x == 10 && y == 8 && wheel == 0);
	/* Movement is available before release, with both axes and real distance. */
	assert(papermono_touchpad_update(&pad, 214, 356, &x, &y, &wheel));
	assert(x == 4 && y == -2);
	assert(papermono_touchpad_update(&pad, 380, 356, &x, &y, &wheel));
	assert(x == 166 && wheel == 0); /* Crossing into the strip cannot change mode. */
	assert(papermono_touchpad_update(&pad, 200, 350, &x, &y, &wheel));
	assert(x == -180 && y == -6);
	assert(!papermono_touchpad_update(&pad, 200, 350, &x, &y, &wheel));
	assert(pad.moved); /* Returning to the start cannot turn a drag into a click. */

	papermono_touchpad_begin(&pad, 400, 400, true);
	assert(!papermono_touchpad_update(&pad, 399, 390, &x, &y, &wheel));
	assert(pad.moved);
	assert(papermono_touchpad_update(&pad, 300, 380, &x, &y, &wheel));
	assert(x == 0 && y == 0 && wheel == 1 && pad.scroll_remainder == 4);
	assert(papermono_touchpad_update(&pad, 300, 368, &x, &y, &wheel));
	assert(wheel == 1 && pad.scroll_remainder == 0);
	assert(papermono_touchpad_update(&pad, 300, 400, &x, &y, &wheel));
	assert(wheel == -2);
	papermono_touchpad_begin(&pad, 200, 350, false);
	assert(!pad.moved && !pad.scrolling && pad.scroll_remainder == 0);
	return 0;
}
