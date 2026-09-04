/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <papermono/touch_gesture.h>

#define WIDTH 480
#define HEIGHT 800

static enum papermono_app_event_type classify(int16_t start_x,
	int16_t start_y, int16_t end_x, int16_t end_y, uint32_t duration_ms)
{
	enum papermono_app_event_type type = PAPERMONO_APP_EVENT_REFRESH;

	assert(papermono_touch_gesture_classify(start_x, start_y, end_x,
		end_y, duration_ms, WIDTH, HEIGHT, &type) == 0);
	return type;
}

int main(void)
{
	enum papermono_app_event_type type;
	int16_t event_x;
	int16_t event_y;

	assert(classify(40, 300, 140, 305, 200) == PAPERMONO_APP_EVENT_BACK);
	assert(classify(200, 750, 205, 650, 200) == PAPERMONO_APP_EVENT_HOME);
	assert(classify(300, 300, 200, 305, 200) == PAPERMONO_APP_EVENT_NEXT);
	assert(classify(200, 300, 300, 305, 200) == PAPERMONO_APP_EVENT_PREVIOUS);
	assert(classify(200, 500, 205, 400, 200) == PAPERMONO_APP_EVENT_UP);
	assert(classify(200, 300, 205, 400, 200) == PAPERMONO_APP_EVENT_DOWN);
	assert(classify(200, 300, 205, 305, 200) == PAPERMONO_APP_EVENT_TAP);
	/* One complete sample at the minimum 8 ms polling cadence is a real tap. */
	assert(classify(200, 300, 200, 300, 8) == PAPERMONO_APP_EVENT_TAP);
	assert(classify(200, 300, 205, 305, 900) ==
		PAPERMONO_APP_EVENT_LONG_PRESS);
	/* An ordinary right swipe outside the edge remains previous-page input. */
	assert(classify(160, 300, 260, 300, 200) ==
		PAPERMONO_APP_EVENT_PREVIOUS);
	/* Short release drift below the swipe threshold remains a usable tap. */
	assert(classify(100, 100, 140, 140, 200) == PAPERMONO_APP_EVENT_TAP);
	assert(classify(200, 100, 259, 100, 200) == PAPERMONO_APP_EVENT_TAP);
	assert(classify(200, 100, 260, 100, 200) ==
		PAPERMONO_APP_EVENT_PREVIOUS);
	/* Long press retains a tighter stationary slop than a released tap. */
	assert(papermono_touch_gesture_classify(100, 100, 140, 140, 900,
		WIDTH, HEIGHT, &type) == -ENOENT);
	assert(papermono_touch_gesture_classify(100, 100, 100, 100, 2,
		WIDTH, HEIGHT, &type) == -EAGAIN);

	assert(papermono_touch_gesture_event_point(PAPERMONO_APP_EVENT_TAP,
		100, 110, 140, 150, &event_x, &event_y) == 0);
	assert(event_x == 100 && event_y == 110);
	assert(papermono_touch_gesture_event_point(
		PAPERMONO_APP_EVENT_LONG_PRESS, 120, 130, 125, 135,
		&event_x, &event_y) == 0);
	assert(event_x == 120 && event_y == 130);
	assert(papermono_touch_gesture_event_point(PAPERMONO_APP_EVENT_NEXT,
		200, 210, 100, 215, &event_x, &event_y) == 0);
	assert(event_x == 200 && event_y == 210);
	assert(papermono_touch_gesture_event_point(PAPERMONO_APP_EVENT_TAP,
		0, 0, 0, 0, NULL, &event_y) == -EINVAL);
	puts("touch gesture classification: PASS");
	return 0;
}
