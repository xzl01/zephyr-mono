/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <papermono/touch_gesture.h>

#define TOUCH_SWIPE_MIN 60
#define TOUCH_EDGE_SWIPE_MIN 32
#define TOUCH_LONG_PRESS_MOVE_MAX 30
#define TOUCH_TAP_RELEASE_MOVE_MAX (TOUCH_SWIPE_MIN - 1)
#define TOUCH_MIN_PRESS_MS 5U
#define TOUCH_LONG_PRESS_MS 800U
#define TOUCH_MAX_PRESS_MS 2500U

int papermono_touch_gesture_navigation(int16_t start_x, int16_t start_y,
	int16_t end_x, int16_t end_y, uint32_t duration_ms,
	int16_t width, int16_t height, enum papermono_app_event_type *type)
{
	const int32_t delta_x = (int32_t)end_x - start_x;
	const int32_t delta_y = (int32_t)end_y - start_y;
	const int32_t abs_x = delta_x < 0 ? -delta_x : delta_x;
	const int32_t abs_y = delta_y < 0 ? -delta_y : delta_y;

	if (type == NULL || width <= 0 || height <= 0 || start_x < 0 ||
	    start_y < 0 || end_x < 0 || end_y < 0 || start_x >= width ||
	    end_x >= width || start_y >= height || end_y >= height) {
		return -EINVAL;
	}
	if (duration_ms < TOUCH_MIN_PRESS_MS ||
	    duration_ms > TOUCH_MAX_PRESS_MS) {
		return -EAGAIN;
	}
	const int32_t back_distance = start_x < width / 10 ?
		TOUCH_EDGE_SWIPE_MIN : TOUCH_SWIPE_MIN;
	/* A thumb naturally travels diagonally when starting against the bezel.
	 * Only the narrow edge accepts this slop; vertical scrolling further in
	 * and ordinary right swipes retain their existing classification.
	 */
	const bool back_direction = start_x < width / 10 ?
		delta_x > 0 && abs_y <= delta_x * 2 : abs_x > abs_y;

	if (delta_x >= back_distance && back_direction &&
	    start_x < width / 4) {
		*type = PAPERMONO_APP_EVENT_BACK;
		return 0;
	}
	if (delta_y <= -TOUCH_SWIPE_MIN && abs_y > abs_x &&
	    start_y >= height - (height * 14) / 100) {
		*type = PAPERMONO_APP_EVENT_HOME;
		return 0;
	}
	return -ENOENT;
}

int papermono_touch_gesture_classify(int16_t start_x, int16_t start_y,
	int16_t end_x, int16_t end_y, uint32_t duration_ms,
	int16_t width, int16_t height, enum papermono_app_event_type *type)
{
	const int ret = papermono_touch_gesture_navigation(start_x, start_y,
		end_x, end_y, duration_ms, width, height, type);
	const int32_t delta_x = (int32_t)end_x - start_x;
	const int32_t delta_y = (int32_t)end_y - start_y;
	const int32_t abs_x = delta_x < 0 ? -delta_x : delta_x;
	const int32_t abs_y = delta_y < 0 ? -delta_y : delta_y;

	if (ret != -ENOENT) {
		return ret;
	}
	if (delta_x <= -TOUCH_SWIPE_MIN && abs_x > abs_y) {
		*type = PAPERMONO_APP_EVENT_NEXT;
		return 0;
	}
	if (delta_x >= TOUCH_SWIPE_MIN && abs_x > abs_y) {
		*type = PAPERMONO_APP_EVENT_PREVIOUS;
		return 0;
	}
	if (delta_y <= -TOUCH_SWIPE_MIN && abs_y > abs_x) {
		*type = PAPERMONO_APP_EVENT_UP;
		return 0;
	}
	if (delta_y >= TOUCH_SWIPE_MIN && abs_y > abs_x) {
		*type = PAPERMONO_APP_EVENT_DOWN;
		return 0;
	}
	if (duration_ms >= TOUCH_LONG_PRESS_MS &&
	    abs_x <= TOUCH_LONG_PRESS_MOVE_MAX &&
	    abs_y <= TOUCH_LONG_PRESS_MOVE_MAX) {
		*type = PAPERMONO_APP_EVENT_LONG_PRESS;
		return 0;
	}
	/* FT6336 coordinates commonly drift while a finger rolls during release.
	 * Accept every short movement below the swipe threshold as a tap so there
	 * is no 31..59 px dead zone between tap and swipe classification.
	 */
	if (duration_ms < TOUCH_LONG_PRESS_MS &&
	    abs_x <= TOUCH_TAP_RELEASE_MOVE_MAX &&
	    abs_y <= TOUCH_TAP_RELEASE_MOVE_MAX) {
		*type = PAPERMONO_APP_EVENT_TAP;
		return 0;
	}
	return -ENOENT;
}

int papermono_touch_gesture_event_point(enum papermono_app_event_type type,
	int16_t start_x, int16_t start_y, int16_t end_x, int16_t end_y,
	int16_t *event_x, int16_t *event_y)
{
	if (event_x == NULL || event_y == NULL) {
		return -EINVAL;
	}

	/* UI regions describe where the gesture began. Anchoring every gesture to
	 * the first contact keeps a diagonal brightness swipe in its card and a
	 * mouse-wheel swipe in the right-side strip even when the release drifts.
	 * Direction remains encoded by the classified event type.
	 */
	(void)type;
	(void)end_x;
	(void)end_y;
	*event_x = start_x;
	*event_y = start_y;
	return 0;
}
