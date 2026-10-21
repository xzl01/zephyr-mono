/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <papermono/touchpad.h>

static void test_click_slop(void)
{
	struct papermono_touchpad pad;
	int16_t x, y, wheel;

	papermono_touchpad_begin(&pad, 200, 350, 0, false);
	assert(!papermono_touchpad_update(&pad, 201, 352, 8, &x, &y, &wheel));
	assert(!pad.tracking && !pad.moved);
	assert(papermono_touchpad_update(&pad, 203, 352, 50, &x, &y, &wheel));
	assert(x == 2 && y == 1 && wheel == 0 && pad.tracking && !pad.moved);
	assert(papermono_touchpad_update(&pad, 207, 352, 100, &x, &y, &wheel));
	assert(pad.moved);
	assert(papermono_touchpad_update(&pad, 200, 350, 200, &x, &y, &wheel));
	assert(x < 0 && y < 0 && pad.moved);
	papermono_touchpad_begin(&pad, 200, 350, 300, false);
	assert(!pad.moved && !pad.tracking && pad.remainder_x == 0);
}

static void test_fractional_motion(void)
{
	struct papermono_touchpad pad;
	int16_t x, y, wheel;
	int total;

	for (int sign = -1; sign <= 1; sign += 2) {
		total = 0;
		papermono_touchpad_begin(&pad, 200, 350, 0, false);
		(void)papermono_touchpad_update(&pad, 200 + sign * 4,
			350, 100, &x, &y, &wheel);
		total += x;
		for (int step = 5; step <= 8; ++step) {
			(void)papermono_touchpad_update(&pad, 200 + sign * step,
				350, (step - 3) * 100, &x, &y, &wheel);
			total += x;
		}
		assert(total == sign * 6); /* Eight slow counts at 0.75x, with no loss. */
		assert(pad.remainder_x == 0);
	}
	papermono_touchpad_begin(&pad, 200, 350, 0, false);
	(void)papermono_touchpad_update(&pad, 203, 350, 100, &x, &y, &wheel);
	assert(x == 2 && !pad.moved);
	(void)papermono_touchpad_update(&pad, 200, 350, 200, &x, &y, &wheel);
	assert(x == -2 && pad.remainder_x == 0 && !pad.moved);
}

static void test_speed_and_sample_cadence(void)
{
	struct papermono_touchpad pad;
	int16_t x, y, wheel;

	/* Equal paths at low, middle and high speeds keep their gain at different
	 * controller cadences. Each case travels sixteen raw counts.
	 */
	const int durations[] = {200, 40, 20};
	const int expected[] = {12, 18, 28};
	for (int speed = 0; speed < 3; ++speed) {
		const int duration = durations[speed];
		for (int period = duration / 4; period <= duration; period *= 2) {
			int total = 0;
			papermono_touchpad_begin(&pad, 200, 350, 0, false);
			for (int now = period; now <= duration; now += period) {
				(void)papermono_touchpad_update(&pad,
					200 + now * 16 / duration, 350, now, &x, &y, &wheel);
				total += x;
			}
			assert(total == expected[speed]);
		}
	}
	/* Duplicate polls cannot falsely boost the next real sample's speed. */
	papermono_touchpad_begin(&pad, 200, 350, 0, false);
	assert(!papermono_touchpad_update(&pad, 200, 350, 49, &x, &y, &wheel));
	(void)papermono_touchpad_update(&pad, 204, 350, 50, &x, &y, &wheel);
	assert(x == 3); /* 80 px/s remains in the fine-positioning range. */
	assert(!papermono_touchpad_update(&pad, 204, 350, 99, &x, &y, &wheel));
	(void)papermono_touchpad_update(&pad, 208, 350, 100, &x, &y, &wheel);
	assert(x == 3);
	/* One gain preserves a diagonal's direction and signed fractional counts. */
	papermono_touchpad_begin(&pad, 200, 350, 0, false);
	(void)papermono_touchpad_update(&pad, 210, 360, 10, &x, &y, &wheel);
	assert(x == 17 && y == 17);
	(void)papermono_touchpad_update(&pad, 220, 370, 20, &x, &y, &wheel);
	assert(x == 18 && y == 18);
	/* Wrap and repeated timestamps never divide by zero or escape the cap. */
	papermono_touchpad_begin(&pad, 200, 350, UINT32_MAX - 3U, false);
	(void)papermono_touchpad_update(&pad, 208, 350, 4, &x, &y, &wheel);
	assert(x == 14);
	(void)papermono_touchpad_update(&pad, 216, 350, 4, &x, &y, &wheel);
	assert(x == 14);
}

static void test_scroll_boundary(void)
{
	struct papermono_touchpad pad;
	int16_t x, y, wheel;

	papermono_touchpad_begin(&pad, 400, 400, 0, true);
	assert(!papermono_touchpad_update(&pad, 399, 390, 8, &x, &y, &wheel));
	assert(pad.moved);
	assert(papermono_touchpad_update(&pad, 300, 380, 16, &x, &y, &wheel));
	assert(x == 0 && y == 0 && wheel == 1 && pad.scroll_remainder == 4);
	assert(papermono_touchpad_update(&pad, 300, 368, 24, &x, &y, &wheel));
	assert(wheel == 1 && pad.scroll_remainder == 0);
	assert(papermono_touchpad_update(&pad, 300, 400, 32, &x, &y, &wheel));
	assert(wheel == -2);
	papermono_touchpad_begin(&pad, 200, 350, 40, false);
	assert(papermono_touchpad_update(&pad, 380, 350, 80, &x, &y, &wheel));
	assert(x > 0 && y == 0 && wheel == 0 && !pad.scrolling);
}

int main(void)
{
	test_click_slop();
	test_fractional_motion();
	test_speed_and_sample_cadence();
	test_scroll_boundary();
	return 0;
}
