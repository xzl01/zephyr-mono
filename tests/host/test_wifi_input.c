/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <papermono/wifi_input.h>

static size_t action_index(const struct papermono_wifi_input *input,
	enum papermono_wifi_input_action action)
{
	for (size_t i = 0U; i < papermono_wifi_input_key_count(); ++i) {
		struct papermono_wifi_input_key key;

		assert(papermono_wifi_input_key_at(input, i, &key) == 0);
		if (key.action == action) {
			return i;
		}
	}
	assert(!"missing action");
	return 0U;
}

static void assert_zero(const void *memory, size_t size)
{
	const unsigned char *bytes = memory;

	for (size_t i = 0U; i < size; ++i) {
		assert(bytes[i] == 0U);
	}
}

static void test_limits_and_visibility(void)
{
	struct {
		unsigned char before[8];
		struct papermono_wifi_input input;
		unsigned char after[8];
	} guarded;
	struct papermono_wifi_input *input = &guarded.input;
	char display[64];

	memset(&guarded, 0xa5, sizeof(guarded));
	for (int field = PAPERMONO_WIFI_INPUT_PASSWORD;
	     field <= PAPERMONO_WIFI_INPUT_SSID; ++field) {
		size_t limit = field == PAPERMONO_WIFI_INPUT_PASSWORD ? 63U : 32U;

		assert(papermono_wifi_input_init(input,
			(enum papermono_wifi_input_field)field) == 0);
		assert(input->limit == limit && input->length == 0U);
		assert(input->visible == (field == PAPERMONO_WIFI_INPUT_SSID));
		assert_zero(input->text, sizeof(input->text));
		assert(papermono_wifi_input_display(input, display, 1U) == 0);
		assert(display[0] == '\0');
		for (size_t i = 0U; i < limit; ++i) {
			assert(papermono_wifi_input_append(input, 'x') == 0);
			assert(input->length == i + 1U);
			assert(input->text[i + 1U] == '\0');
		}
		assert(papermono_wifi_input_append(input, 'y') == -ENOSPC);
		assert(input->length == limit && strlen(input->text) == limit);
		assert(papermono_wifi_input_tap(input, 10, 360) == -ENOSPC);
		assert(papermono_wifi_input_display(input, display, limit + 1U) == 0);
		for (size_t i = 0U; i < limit; ++i) {
			assert(display[i] == (input->visible ? 'x' : '*'));
		}
		assert(papermono_wifi_input_display(input, display, limit) == -ENOSPC);
		assert(display[0] == '\0');
		assert(papermono_wifi_input_display(input, display, 0U) == -ENOSPC);
		assert(papermono_wifi_input_press(input,
			action_index(input, PAPERMONO_WIFI_INPUT_VISIBILITY)) == 0);
		assert(input->visible == (field == PAPERMONO_WIFI_INPUT_PASSWORD));
		assert(papermono_wifi_input_display(input, display, sizeof(display)) == 0);
		assert(display[0] == (input->visible ? 'x' : '*'));
		assert(papermono_wifi_input_backspace(input) == 0);
		assert(input->length == limit - 1U);
		assert_zero(input->text + input->length,
			sizeof(input->text) - input->length);
		assert(papermono_wifi_input_append(input, 'z') == 0);
		assert(input->text[limit - 1U] == 'z');
		assert(papermono_wifi_input_press(input,
			action_index(input, PAPERMONO_WIFI_INPUT_CLEAR)) == 0);
		assert(input->length == 0U && input->limit == limit);
		assert(input->visible == (field == PAPERMONO_WIFI_INPUT_SSID));
		assert_zero(input->text, sizeof(input->text));
		assert(papermono_wifi_input_backspace(input) == 0);
		assert(input->length == 0U);
		for (size_t i = 0U; i < sizeof(guarded.before); ++i) {
			assert(guarded.before[i] == 0xa5U);
			assert(guarded.after[i] == 0xa5U);
		}
	}
}

static void test_all_ascii_and_layers(void)
{
	struct papermono_wifi_input input;
	bool reachable[127] = { false };
	char display[64];

	assert(papermono_wifi_input_init(&input, PAPERMONO_WIFI_INPUT_PASSWORD) == 0);
	for (int layer = PAPERMONO_WIFI_INPUT_LOWER;
	     layer <= PAPERMONO_WIFI_INPUT_SYMBOL; ++layer) {
		/* Reach each layer using real key events, not by mutating state. */
		if (layer == PAPERMONO_WIFI_INPUT_UPPER) {
			assert(papermono_wifi_input_press(&input,
				action_index(&input, PAPERMONO_WIFI_INPUT_SHIFT)) == 0);
		} else if (layer == PAPERMONO_WIFI_INPUT_SYMBOL) {
			assert(papermono_wifi_input_press(&input,
				action_index(&input, PAPERMONO_WIFI_INPUT_SYMBOLS)) == 0);
		}
		assert(input.layer == (enum papermono_wifi_input_layer)layer);
		for (size_t i = 0U; i < PAPERMONO_WIFI_INPUT_CHARACTER_KEYS; ++i) {
			struct papermono_wifi_input_key key;

			assert(papermono_wifi_input_key_at(&input, i, &key) == 0);
			assert(key.action == PAPERMONO_WIFI_INPUT_CHARACTER);
			assert(key.character >= 32 && key.character <= 126);
			assert(key.label != NULL && key.label[0] != '\0');
			if (key.character == ' ') {
				assert(strcmp(key.label, "空格") == 0);
			} else {
				assert(key.label[0] == key.character && key.label[1] == '\0');
			}
			reachable[(unsigned char)key.character] = true;
			papermono_wifi_input_clear(&input);
			assert(papermono_wifi_input_tap(&input,
				key.x + key.width / 2, key.y + key.height / 2) == 1);
			assert(input.length == 1U && input.text[0] == key.character);
			assert(input.text[1] == '\0');
			assert(papermono_wifi_input_display(&input, display, sizeof(display)) == 0);
			assert(strcmp(display, "*") == 0);
		}
	}
	for (int c = 32; c <= 126; ++c) {
		assert(reachable[c]);
	}
	/* In particular spaces, quotes and a backslash must not be lost. */
	papermono_wifi_input_clear(&input);
	const char *special = " \\\"'";
	for (const char *p = special; *p != '\0'; ++p) {
		assert(papermono_wifi_input_append(&input, *p) == 0);
	}
	assert(strcmp(input.text, special) == 0);
	assert(papermono_wifi_input_press(&input,
		action_index(&input, PAPERMONO_WIFI_INPUT_SYMBOLS)) == 0);
	assert(input.layer == PAPERMONO_WIFI_INPUT_LOWER);
	assert(papermono_wifi_input_press(&input,
		action_index(&input, PAPERMONO_WIFI_INPUT_SHIFT)) == 0);
	assert(input.layer == PAPERMONO_WIFI_INPUT_UPPER);
	assert(papermono_wifi_input_press(&input,
		action_index(&input, PAPERMONO_WIFI_INPUT_SHIFT)) == 0);
	assert(input.layer == PAPERMONO_WIFI_INPUT_LOWER);
	for (int c = 0; c <= 255; ++c) {
		papermono_wifi_input_clear(&input);
		int rc = papermono_wifi_input_append(&input, (char)c);

		assert(rc == (c >= 32 && c <= 126 ? 0 : -EINVAL));
		assert(input.length == (c >= 32 && c <= 126 ? 1U : 0U));
	}
}

static void test_geometry_and_gaps(void)
{
	struct papermono_wifi_input input;
	struct papermono_wifi_input_key keys[PAPERMONO_WIFI_INPUT_KEY_COUNT];

	assert(papermono_wifi_input_init(&input, PAPERMONO_WIFI_INPUT_PASSWORD) == 0);
	assert(papermono_wifi_input_key_count() == 45U);
	for (size_t i = 0U; i < papermono_wifi_input_key_count(); ++i) {
		struct papermono_wifi_input_key *key = &keys[i];

		assert(papermono_wifi_input_key_at(&input, i, key) == 0);
		assert(key->width >= 40 && key->height >= 55);
		assert(key->x >= 0 && key->x + key->width <= 480);
		assert(key->y >= 360 && key->y + key->height <= 690);
		if (i < PAPERMONO_WIFI_INPUT_CHARACTER_KEYS) {
			assert(key->x == 10 + (int)(i % 10U) * 46);
			assert(key->y == 360 + (int)(i / 10U) * 66);
		}
		const int corners[4][2] = {
			{ key->x, key->y },
			{ key->x + key->width - 1, key->y },
			{ key->x, key->y + key->height - 1 },
			{ key->x + key->width - 1, key->y + key->height - 1 },
		};
		for (size_t j = 0U; j < 4U; ++j) {
			papermono_wifi_input_clear(&input);
			assert(papermono_wifi_input_tap(&input, corners[j][0], corners[j][1]) == 1);
		}
		/* All layers share exactly the same hit rectangles. */
		for (int layer = PAPERMONO_WIFI_INPUT_LOWER;
		     layer <= PAPERMONO_WIFI_INPUT_SYMBOL; ++layer) {
			struct papermono_wifi_input_key other;

			input.layer = (enum papermono_wifi_input_layer)layer;
			assert(papermono_wifi_input_key_at(&input, i, &other) == 0);
			assert(other.x == key->x && other.y == key->y);
			assert(other.width == key->width && other.height == key->height);
		}
	}
	/* Exhaustively check every unoccupied pixel, including all 2px/4px/6px
	 * gaps and everything outside the keyboard on the 480x800 screen.
	 */
	for (int y = 0; y < 800; ++y) {
		for (int x = 0; x < 480; ++x) {
			unsigned int hits = 0U;

			for (size_t i = 0U; i < PAPERMONO_WIFI_INPUT_KEY_COUNT; ++i) {
				const struct papermono_wifi_input_key *key = &keys[i];

				hits += x >= key->x && x < key->x + key->width &&
					y >= key->y && y < key->y + key->height;
			}
			assert(hits <= 1U);
			if (hits == 0U) {
				struct papermono_wifi_input saved = input;

				assert(papermono_wifi_input_tap(&input, x, y) == 0);
				assert(memcmp(&input, &saved, sizeof(input)) == 0);
			}
		}
	}
	assert(papermono_wifi_input_tap(&input, -1, 360) == 0);
	assert(papermono_wifi_input_tap(&input, 10, -1) == 0);
	assert(papermono_wifi_input_tap(&input, 480, 360) == 0);
	assert(papermono_wifi_input_tap(&input, 10, 800) == 0);
	assert(papermono_wifi_input_tap(&input, INT_MIN, INT_MAX) == 0);
	assert(papermono_wifi_input_tap(&input, INT_MAX, INT_MIN) == 0);
}

static void test_erasure_and_invalid_arguments(void)
{
	struct papermono_wifi_input input;
	struct papermono_wifi_input_key key;
	char display[64];

	memset(&input, 0xa5, sizeof(input));
	papermono_wifi_input_wipe(&input);
	assert_zero(&input, sizeof(input));
	assert(papermono_wifi_input_append(&input, 'a') == -EINVAL);
	assert(papermono_wifi_input_init(&input, PAPERMONO_WIFI_INPUT_PASSWORD) == 0);
	assert(papermono_wifi_input_append(&input, 's') == 0);
	/* Even stale data beyond length must be erased by clear/backspace. */
	memset(input.text + 2, 'x', sizeof(input.text) - 2U);
	assert(papermono_wifi_input_backspace(&input) == 0);
	assert_zero(input.text, sizeof(input.text));
	assert(papermono_wifi_input_append(&input, 's') == 0);
	memset(input.text + 2, 'x', sizeof(input.text) - 2U);
	papermono_wifi_input_clear(&input);
	assert_zero(input.text, sizeof(input.text));
	assert(papermono_wifi_input_append(&input, 's') == 0);
	assert(papermono_wifi_input_init(&input, PAPERMONO_WIFI_INPUT_SSID) == 0);
	assert_zero(input.text, sizeof(input.text));
	assert(papermono_wifi_input_append(&input, 's') == 0);
	papermono_wifi_input_wipe(&input);
	assert_zero(&input, sizeof(input));
	assert(papermono_wifi_input_init(NULL, PAPERMONO_WIFI_INPUT_PASSWORD) == -EINVAL);
	assert(papermono_wifi_input_init(&input,
		(enum papermono_wifi_input_field)99) == -EINVAL);
	assert_zero(&input, sizeof(input));
	assert(papermono_wifi_input_init(&input, PAPERMONO_WIFI_INPUT_PASSWORD) == 0);
	assert(papermono_wifi_input_append(NULL, 'a') == -EINVAL);
	assert(papermono_wifi_input_backspace(NULL) == -EINVAL);
	assert(papermono_wifi_input_display(NULL, display, sizeof(display)) == -EINVAL);
	assert(papermono_wifi_input_display(&input, NULL, 0U) == -EINVAL);
	assert(papermono_wifi_input_key_at(NULL, 0U, &key) == -EINVAL);
	assert(papermono_wifi_input_key_at(&input, 0U, NULL) == -EINVAL);
	assert(papermono_wifi_input_key_at(&input, 45U, &key) == -ERANGE);
	assert(papermono_wifi_input_press(&input, 45U) == -ERANGE);
	assert(papermono_wifi_input_press(NULL, 0U) == -EINVAL);
	assert(papermono_wifi_input_tap(NULL, 10, 360) == -EINVAL);
	input.length = sizeof(input.text);
	assert(papermono_wifi_input_backspace(&input) == -EINVAL);
	assert(papermono_wifi_input_display(&input, display, sizeof(display)) == -EINVAL);
	papermono_wifi_input_clear(NULL);
	papermono_wifi_input_wipe(NULL);
}

int main(void)
{
	test_limits_and_visibility();
	test_all_ascii_and_layers();
	test_geometry_and_gaps();
	test_erasure_and_invalid_arguments();
	puts("Paper Mono Wi-Fi input: PASS");
	return 0;
}
