/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <papermono/hid.h>

static uint32_t item_value(const uint8_t *data, size_t length)
{
	uint32_t value = 0U;

	for (size_t index = 0U; index < length; ++index) {
		value |= (uint32_t)data[index] << (index * 8U);
	}
	return value;
}

static void test_report_map_lengths(void)
{
	uint32_t input_bits[4] = { 0U };
	uint32_t report_size = 0U;
	uint32_t report_count = 0U;
	uint32_t report_id = 0U;
	size_t length;
	const uint8_t *map = papermono_hid_report_map(&length);
	size_t offset = 0U;

	assert(map != NULL);
	assert(length > 0U);
	assert(papermono_hid_report_map(NULL) == map);
	while (offset < length) {
		const uint8_t prefix = map[offset++];
		const size_t size_code = prefix & 0x03U;
		const size_t item_length = size_code == 3U ? 4U : size_code;
		const uint8_t type = (prefix >> 2U) & 0x03U;
		const uint8_t tag = prefix >> 4U;
		uint32_t value;

		assert(prefix != 0xFEU); /* No HID long items in this map. */
		assert(offset + item_length <= length);
		value = item_value(&map[offset], item_length);
		offset += item_length;
		if (type == 1U && tag == 7U) {
			report_size = value;
		} else if (type == 1U && tag == 8U) {
			report_id = value;
			assert(report_id > 0U && report_id < 4U);
		} else if (type == 1U && tag == 9U) {
			report_count = value;
		} else if (type == 0U && tag == 8U) {
			assert(report_id > 0U && report_id < 4U);
			assert(report_size > 0U && report_count > 0U);
			input_bits[report_id] += report_size * report_count;
		}
	}
	assert(input_bits[PAPERMONO_HID_REPORT_MEDIA] ==
		PAPERMONO_HID_MEDIA_REPORT_SIZE * 8U);
	assert(input_bits[PAPERMONO_HID_REPORT_KEYBOARD] ==
		PAPERMONO_HID_KEYBOARD_REPORT_SIZE * 8U);
	assert(input_bits[PAPERMONO_HID_REPORT_MOUSE] ==
		PAPERMONO_HID_MOUSE_REPORT_SIZE * 8U);
}

static void test_media_reports(void)
{
	static const enum papermono_hid_action_type types[] = {
		PAPERMONO_HID_MEDIA_PLAY_PAUSE,
		PAPERMONO_HID_MEDIA_PREVIOUS,
		PAPERMONO_HID_MEDIA_NEXT,
		PAPERMONO_HID_MEDIA_VOLUME_UP,
		PAPERMONO_HID_MEDIA_VOLUME_DOWN,
		PAPERMONO_HID_MEDIA_MUTE,
	};

	for (size_t index = 0U; index < sizeof(types) / sizeof(types[0]); ++index) {
		const struct papermono_hid_action action = { .type = types[index] };
		struct papermono_hid_report report;

		assert(papermono_hid_encode_action(&action, &report) == 0);
		assert(report.id == PAPERMONO_HID_REPORT_MEDIA);
		assert(report.length == PAPERMONO_HID_MEDIA_REPORT_SIZE);
		assert(report.data[0] == (uint8_t)(1U << index));
		assert(report.release_required);
		for (size_t byte = 1U; byte < sizeof(report.data); ++byte) {
			assert(report.data[byte] == 0U);
		}
	}
}

static void test_presentation_reports(void)
{
	static const struct {
		enum papermono_hid_action_type type;
		uint8_t key;
	} cases[] = {
		{ PAPERMONO_HID_PRESENT_PREVIOUS, 0x4BU },
		{ PAPERMONO_HID_PRESENT_NEXT, 0x4EU },
		{ PAPERMONO_HID_PRESENT_START, 0x3EU },
		{ PAPERMONO_HID_PRESENT_EXIT, 0x29U },
		{ PAPERMONO_HID_PRESENT_BLACK, 0x05U },
	};

	for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		const struct papermono_hid_action action = { .type = cases[index].type };
		struct papermono_hid_report report;

		assert(papermono_hid_encode_action(&action, &report) == 0);
		assert(report.id == PAPERMONO_HID_REPORT_KEYBOARD);
		assert(report.length == PAPERMONO_HID_KEYBOARD_REPORT_SIZE);
		assert(report.data[0] == 0U);
		assert(report.data[1] == 0U);
		assert(report.data[2] == cases[index].key);
		assert(report.release_required);
		for (size_t byte = 3U; byte < sizeof(report.data); ++byte) {
			assert(report.data[byte] == 0U);
		}
	}
}

static void test_mouse_reports(void)
{
	struct papermono_hid_action action = {
		.type = PAPERMONO_HID_MOUSE,
		.mouse_x = -40,
		.mouse_y = 31,
		.wheel = -1,
	};
	struct papermono_hid_report report;

	assert(papermono_hid_encode_action(&action, &report) == 0);
	assert(report.id == PAPERMONO_HID_REPORT_MOUSE);
	assert(report.length == PAPERMONO_HID_MOUSE_REPORT_SIZE);
	assert(report.data[0] == 0U);
	assert((int8_t)report.data[1] == -40);
	assert((int8_t)report.data[2] == 31);
	assert((int8_t)report.data[3] == -1);
	assert(!report.release_required);

	action.buttons = 0x05U;
	assert(papermono_hid_encode_action(&action, &report) == 0);
	assert(report.data[0] == 0x05U);
	assert(report.release_required);

	action.buttons = 0x08U;
	assert(papermono_hid_encode_action(&action, &report) == -ERANGE);
}

static void test_invalid_input(void)
{
	struct papermono_hid_action action = {
		.type = (enum papermono_hid_action_type)99,
	};
	struct papermono_hid_report report;

	assert(papermono_hid_encode_action(NULL, &report) == -EINVAL);
	assert(papermono_hid_encode_action(&action, NULL) == -EINVAL);
	memset(&report, 0xA5, sizeof(report));
	assert(papermono_hid_encode_action(&action, &report) == -EINVAL);
	for (size_t byte = 0U; byte < sizeof(report.data); ++byte) {
		assert(report.data[byte] == 0U);
	}
}

int main(void)
{
	test_report_map_lengths();
	test_media_reports();
	test_presentation_reports();
	test_mouse_reports();
	test_invalid_input();
	puts("BLE HID report contract tests: PASS");
	return 0;
}
