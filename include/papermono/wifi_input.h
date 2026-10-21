/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_WIFI_INPUT_H_
#define PAPERMONO_WIFI_INPUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_WIFI_INPUT_PASSWORD_MAX 63U
#define PAPERMONO_WIFI_INPUT_SSID_MAX 32U
#define PAPERMONO_WIFI_INPUT_CHARACTER_KEYS 40U
#define PAPERMONO_WIFI_INPUT_KEY_COUNT 45U

/* Fixed portrait 480x800 layout. All bounds are half-open. */
#define PAPERMONO_WIFI_INPUT_TOP 360
#define PAPERMONO_WIFI_INPUT_BOTTOM 690

enum papermono_wifi_input_field {
	PAPERMONO_WIFI_INPUT_PASSWORD,
	PAPERMONO_WIFI_INPUT_SSID,
};

enum papermono_wifi_input_layer {
	PAPERMONO_WIFI_INPUT_LOWER,
	PAPERMONO_WIFI_INPUT_UPPER,
	PAPERMONO_WIFI_INPUT_SYMBOL,
};

enum papermono_wifi_input_action {
	PAPERMONO_WIFI_INPUT_CHARACTER,
	PAPERMONO_WIFI_INPUT_SHIFT,
	PAPERMONO_WIFI_INPUT_SYMBOLS,
	PAPERMONO_WIFI_INPUT_BACKSPACE,
	PAPERMONO_WIFI_INPUT_CLEAR,
	PAPERMONO_WIFI_INPUT_VISIBILITY,
};

struct papermono_wifi_input {
	char text[PAPERMONO_WIFI_INPUT_PASSWORD_MAX + 1U];
	size_t length;
	size_t limit;
	enum papermono_wifi_input_field field;
	enum papermono_wifi_input_layer layer;
	bool visible;
};

/* label points to immutable UTF-8/static storage. character is meaningful
 * only for CHARACTER actions. Use these exact rectangles for drawing and taps.
 */
struct papermono_wifi_input_key {
	int16_t x;
	int16_t y;
	int16_t width;
	int16_t height;
	enum papermono_wifi_input_action action;
	char character;
	const char *label;
};

/* All int APIs return 0 on success or a negative errno, except tap below.
 * Initialization securely erases prior storage; password starts masked and
 * SSID starts visible. Text accepts only ASCII 32..126, with a trailing NUL.
 */
int papermono_wifi_input_init(struct papermono_wifi_input *input,
	enum papermono_wifi_input_field field);
int papermono_wifi_input_append(struct papermono_wifi_input *input, char character);
int papermono_wifi_input_backspace(struct papermono_wifi_input *input);

/* Clear securely erases the entire text array, preserving field and layer,
 * and restores the field's default visibility. wipe erases the ENTIRE model;
 * call it on cancel, leaving the screen, or after consuming credentials, then
 * initialize before reusing. Both accept NULL as a no-op.
 */
void papermono_wifi_input_clear(struct papermono_wifi_input *input);
void papermono_wifi_input_wipe(struct papermono_wifi_input *input);

/* Copies visible text or one '*' per byte. Requires length+1 bytes; on
 * -ENOSPC, out[0] is NUL (no partial credential). Caller owns and must erase
 * any display/copy buffers that may have contained visible credentials.
 */
int papermono_wifi_input_display(const struct papermono_wifi_input *input,
	char *out, size_t capacity);

/* Row-major 4x10 character keys, followed by Shift, Symbols, Backspace,
 * Clear, Visibility. Character keys are 44x60 with 2px horizontal/6px
 * vertical gaps; bottom controls are 88x60 with 4px gaps. All fit y360..690.
 */
size_t papermono_wifi_input_key_count(void);
int papermono_wifi_input_key_at(const struct papermono_wifi_input *input,
	size_t index, struct papermono_wifi_input_key *key);
int papermono_wifi_input_press(struct papermono_wifi_input *input, size_t index);

/* Returns 1 for an applied key, 0 for a miss (including all key gaps), or
 * negative errno for a rejected key, e.g. -ENOSPC when the field is full.
 * Accepts signed coordinates so off-screen negatives are never clamped.
 */
int papermono_wifi_input_tap(struct papermono_wifi_input *input, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_WIFI_INPUT_H_ */
