/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>
#include <papermono/touchpad.h>

static int pointer_gain(int x, int y, uint32_t elapsed_ms)
{
	const unsigned ax = abs(x), ay = abs(y);
	/* Approximate length without floating point; apply one gain to both axes
	 * so diagonals keep their direction. Duplicate controller samples do not
	 * shorten elapsed time or falsely boost the next nonzero displacement.
	 */
	const unsigned length = ax > ay ? ax + ay / 2U : ay + ax / 2U;
	const uint32_t speed = length * 1000U / (elapsed_ms == 0U ? 1U : elapsed_ms);

	if (speed <= 120U) {
		return 192; /* 0.75x for fine positioning. */
	}
	if (speed >= 800U) {
		return 448; /* Cap at 1.75x; the host may also apply acceleration. */
	}
	return 192 + (speed - 120U) * 256U / 680U;
}

static int16_t scale_motion(int delta, int gain, int16_t *remainder)
{
	const int scaled = delta * gain + *remainder;

	*remainder = scaled % 256;
	return scaled / 256;
}

void papermono_touchpad_begin(struct papermono_touchpad *pad,
	int16_t x, int16_t y, uint32_t now_ms, bool scrolling)
{
	*pad = (struct papermono_touchpad) {
		.start_x = x, .start_y = y, .last_x = x, .last_y = y,
		.last_motion_ms = now_ms,
		.scrolling = scrolling,
	};
}

bool papermono_touchpad_update(struct papermono_touchpad *pad,
	int16_t x, int16_t y, uint32_t now_ms,
	int16_t *dx, int16_t *dy, int16_t *wheel)
{
	*dx = *dy = *wheel = 0;
	if (abs(x - pad->start_x) > 6 || abs(y - pad->start_y) > 6) {
		pad->moved = true;
	}
	if (!pad->tracking) {
		/* Start fine motion before exceeding click slop. A small finger roll
		 * can move the pointer slightly and still complete a deliberate tap.
		 */
		if (abs(x - pad->start_x) <= 2 && abs(y - pad->start_y) <= 2) {
			return false;
		}
		pad->tracking = true;
	}
	const int raw_x = x - pad->last_x;
	const int raw_y = y - pad->last_y;

	if (pad->scrolling) {
		pad->scroll_remainder += pad->last_y - y;
		*wheel = pad->scroll_remainder / 16;
		pad->scroll_remainder %= 16;
	} else {
		const int gain = pointer_gain(raw_x, raw_y,
			now_ms - pad->last_motion_ms);

		*dx = scale_motion(raw_x, gain, &pad->remainder_x);
		*dy = scale_motion(raw_y, gain, &pad->remainder_y);
	}
	if (raw_x != 0 || raw_y != 0) {
		pad->last_motion_ms = now_ms;
	}
	pad->last_x = x;
	pad->last_y = y;
	return *dx != 0 || *dy != 0 || *wheel != 0;
}
