/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_HID_H_
#define PAPERMONO_HID_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_hid_action_type {
	PAPERMONO_HID_MEDIA_PLAY_PAUSE,
	PAPERMONO_HID_MEDIA_PREVIOUS,
	PAPERMONO_HID_MEDIA_NEXT,
	PAPERMONO_HID_MEDIA_VOLUME_UP,
	PAPERMONO_HID_MEDIA_VOLUME_DOWN,
	PAPERMONO_HID_MEDIA_MUTE,
	PAPERMONO_HID_PRESENT_PREVIOUS,
	PAPERMONO_HID_PRESENT_NEXT,
	PAPERMONO_HID_PRESENT_START,
	PAPERMONO_HID_PRESENT_EXIT,
	PAPERMONO_HID_PRESENT_BLACK,
	PAPERMONO_HID_READER_PREVIOUS,
	PAPERMONO_HID_READER_NEXT,
	PAPERMONO_HID_MOUSE,
};

enum papermono_hid_report_id {
	PAPERMONO_HID_REPORT_MEDIA = 1,
	PAPERMONO_HID_REPORT_KEYBOARD = 2,
	PAPERMONO_HID_REPORT_MOUSE = 3,
};

#define PAPERMONO_HID_REPORT_MAX_SIZE 8U
#define PAPERMONO_HID_MEDIA_REPORT_SIZE 1U
#define PAPERMONO_HID_KEYBOARD_REPORT_SIZE 8U
#define PAPERMONO_HID_MOUSE_REPORT_SIZE 4U

struct papermono_hid_action {
	enum papermono_hid_action_type type;
	int8_t mouse_x;
	int8_t mouse_y;
	int8_t wheel;
	uint8_t buttons;
};

/** A report payload for one HIDS Report characteristic.
 *
 * The Report ID is carried by that characteristic's Report Reference
 * descriptor, so data contains only the report payload and never an ID byte.
 */
struct papermono_hid_report {
	uint8_t data[PAPERMONO_HID_REPORT_MAX_SIZE];
	uint8_t id;
	uint8_t length;
	bool release_required;
};

struct papermono_hid_state {
	uint32_t passkey;
	int last_error;
	bool initialized;
	bool advertising;
	bool connected;
	bool secured;
	bool bonded;
	bool suspended;
	bool passkey_visible;
};

int papermono_hid_init(void);
const uint8_t *papermono_hid_report_map(size_t *length);
int papermono_hid_encode_action(const struct papermono_hid_action *action,
	struct papermono_hid_report *report);
int papermono_hid_send_action(const struct papermono_hid_action *action);
int papermono_hid_clear_bonds(void);
int papermono_hid_set_battery_level(uint8_t percent);
void papermono_hid_get_state(struct papermono_hid_state *state);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_HID_H_ */
