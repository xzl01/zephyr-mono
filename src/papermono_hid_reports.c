/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <papermono/hid.h>

static const uint8_t report_map[] = {
	/* Report 1: consumer media controls, one bit per action. */
	0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85,
	PAPERMONO_HID_REPORT_MEDIA,
	0x09, 0xCD, 0x09, 0xB6, 0x09, 0xB5, 0x09, 0xE9,
	0x09, 0xEA, 0x09, 0xE2, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x06, 0x81, 0x02,
	0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0xC0,
	/* Report 2: standard keyboard report used for presentation shortcuts. */
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85,
	PAPERMONO_HID_REPORT_KEYBOARD,
	0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
	0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
	0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
	/* Report 3: buttons, relative X/Y, and vertical wheel. */
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85,
	PAPERMONO_HID_REPORT_MOUSE,
	0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
	0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01,
	0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
	0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
	0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0,
};

const uint8_t *papermono_hid_report_map(size_t *length)
{
	if (length != NULL) {
		*length = sizeof(report_map);
	}
	return report_map;
}

static int encode_media(uint8_t bit, struct papermono_hid_report *report)
{
	report->id = PAPERMONO_HID_REPORT_MEDIA;
	report->length = PAPERMONO_HID_MEDIA_REPORT_SIZE;
	report->data[0] = bit;
	report->release_required = true;
	return 0;
}

static int encode_keyboard(uint8_t key, struct papermono_hid_report *report)
{
	report->id = PAPERMONO_HID_REPORT_KEYBOARD;
	report->length = PAPERMONO_HID_KEYBOARD_REPORT_SIZE;
	report->data[2] = key;
	report->release_required = true;
	return 0;
}

int papermono_hid_encode_action(const struct papermono_hid_action *action,
	struct papermono_hid_report *report)
{
	if (action == NULL || report == NULL) {
		return -EINVAL;
	}
	memset(report, 0, sizeof(*report));
	switch (action->type) {
	case PAPERMONO_HID_MEDIA_PLAY_PAUSE:
		return encode_media(1U << 0, report);
	case PAPERMONO_HID_MEDIA_PREVIOUS:
		return encode_media(1U << 1, report);
	case PAPERMONO_HID_MEDIA_NEXT:
		return encode_media(1U << 2, report);
	case PAPERMONO_HID_MEDIA_VOLUME_UP:
		return encode_media(1U << 3, report);
	case PAPERMONO_HID_MEDIA_VOLUME_DOWN:
		return encode_media(1U << 4, report);
	case PAPERMONO_HID_MEDIA_MUTE:
		return encode_media(1U << 5, report);
	case PAPERMONO_HID_PRESENT_PREVIOUS:
		return encode_keyboard(0x4BU, report); /* Page Up */
	case PAPERMONO_HID_PRESENT_NEXT:
		return encode_keyboard(0x4EU, report); /* Page Down */
	case PAPERMONO_HID_PRESENT_START:
		return encode_keyboard(0x3EU, report); /* F5 */
	case PAPERMONO_HID_PRESENT_EXIT:
		return encode_keyboard(0x29U, report); /* Escape */
	case PAPERMONO_HID_PRESENT_BLACK:
		return encode_keyboard(0x05U, report); /* B */
	case PAPERMONO_HID_MOUSE:
		if ((action->buttons & ~0x07U) != 0U) {
			return -ERANGE;
		}
		report->id = PAPERMONO_HID_REPORT_MOUSE;
		report->length = PAPERMONO_HID_MOUSE_REPORT_SIZE;
		report->data[0] = action->buttons;
		report->data[1] = (uint8_t)action->mouse_x;
		report->data[2] = (uint8_t)action->mouse_y;
		report->data[3] = (uint8_t)action->wheel;
		report->release_required = action->buttons != 0U;
		return 0;
	default:
		return -EINVAL;
	}
}
