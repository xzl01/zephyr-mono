/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_TOUCH_GESTURE_H_
#define PAPERMONO_TOUCH_GESTURE_H_

#include <stdint.h>

#include <papermono/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Classify one completed touch sequence into a UI event.
 *
 * The two edge gestures are global: a right swipe beginning in the left
 * quarter is Back, and an upward swipe beginning in the bottom 14 percent is
 * Home. Other swipes remain available to the current application.
 */
int papermono_touch_gesture_classify(int16_t start_x, int16_t start_y,
	int16_t end_x, int16_t end_y, uint32_t duration_ms,
	int16_t width, int16_t height, enum papermono_app_event_type *type);

/**
 * Select the UI coordinate associated with a classified gesture.
 *
 * Every gesture uses the first contact point for region routing, so release
 * drift cannot move a hit into another control or out of its originating
 * swipe area. The classified event type carries the completed direction.
 */
int papermono_touch_gesture_event_point(enum papermono_app_event_type type,
	int16_t start_x, int16_t start_y, int16_t end_x, int16_t end_y,
	int16_t *event_x, int16_t *event_y);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_TOUCH_GESTURE_H_ */
