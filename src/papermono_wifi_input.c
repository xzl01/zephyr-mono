/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <papermono/wifi_input.h>

/* Each string remains stable across layer switches, including quote/backslash.
 * The symbol layer holds all 32 ASCII punctuation characters plus space.
 */
static const char *const characters[][PAPERMONO_WIFI_INPUT_CHARACTER_KEYS] = {
	[PAPERMONO_WIFI_INPUT_LOWER] = {
		"1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
		"q", "w", "e", "r", "t", "y", "u", "i", "o", "p",
		"a", "s", "d", "f", "g", "h", "j", "k", "l", "'",
		"z", "x", "c", "v", "b", "n", "m", " ", ".", ",",
	},
	[PAPERMONO_WIFI_INPUT_UPPER] = {
		"1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
		"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P",
		"A", "S", "D", "F", "G", "H", "J", "K", "L", "'",
		"Z", "X", "C", "V", "B", "N", "M", " ", ".", ",",
	},
	[PAPERMONO_WIFI_INPUT_SYMBOL] = {
		"!", "\"", "#", "$", "%", "&", "'", "(", ")", "*",
		"+", ",", "-", ".", "/", ":", ";", "<", "=", ">",
		"?", "@", "[", "\\", "]", "^", "_", "`", "{", "|",
		"}", "~", " ", "0", "1", "2", "3", "4", "5", "6",
	},
};

static void secure_zero(void *memory, size_t size)
{
	volatile unsigned char *bytes = memory;

	while (size > 0U) {
		*bytes++ = 0U;
		--size;
	}
}

static bool valid(const struct papermono_wifi_input *input)
{
	if (input == NULL || input->layer < PAPERMONO_WIFI_INPUT_LOWER ||
	    input->layer > PAPERMONO_WIFI_INPUT_SYMBOL) {
		return false;
	}
	if ((input->field == PAPERMONO_WIFI_INPUT_PASSWORD &&
	     input->limit != PAPERMONO_WIFI_INPUT_PASSWORD_MAX) ||
	    (input->field == PAPERMONO_WIFI_INPUT_SSID &&
	     input->limit != PAPERMONO_WIFI_INPUT_SSID_MAX) ||
	    (input->field != PAPERMONO_WIFI_INPUT_PASSWORD &&
	     input->field != PAPERMONO_WIFI_INPUT_SSID)) {
		return false;
	}
	return input->length <= input->limit &&
	       input->text[input->length] == '\0';
}

void papermono_wifi_input_wipe(struct papermono_wifi_input *input)
{
	if (input != NULL) {
		secure_zero(input, sizeof(*input));
	}
}

int papermono_wifi_input_init(struct papermono_wifi_input *input,
	enum papermono_wifi_input_field field)
{
	if (input == NULL) {
		return -EINVAL;
	}
	papermono_wifi_input_wipe(input);
	if (field != PAPERMONO_WIFI_INPUT_PASSWORD &&
	    field != PAPERMONO_WIFI_INPUT_SSID) {
		return -EINVAL;
	}
	input->field = field;
	input->limit = field == PAPERMONO_WIFI_INPUT_PASSWORD ?
		PAPERMONO_WIFI_INPUT_PASSWORD_MAX : PAPERMONO_WIFI_INPUT_SSID_MAX;
	input->layer = PAPERMONO_WIFI_INPUT_LOWER;
	input->visible = field == PAPERMONO_WIFI_INPUT_SSID;
	return 0;
}

int papermono_wifi_input_append(struct papermono_wifi_input *input, char character)
{
	unsigned char byte = (unsigned char)character;

	if (!valid(input) || byte < 32U || byte > 126U) {
		return -EINVAL;
	}
	if (input->length == input->limit) {
		return -ENOSPC;
	}
	input->text[input->length++] = character;
	input->text[input->length] = '\0';
	return 0;
}

int papermono_wifi_input_backspace(struct papermono_wifi_input *input)
{
	if (!valid(input)) {
		return -EINVAL;
	}
	if (input->length > 0U) {
		--input->length;
	}
	secure_zero(input->text + input->length,
		sizeof(input->text) - input->length);
	return 0;
}

void papermono_wifi_input_clear(struct papermono_wifi_input *input)
{
	if (input != NULL) {
		secure_zero(input->text, sizeof(input->text));
		input->length = 0U;
		input->visible = input->field == PAPERMONO_WIFI_INPUT_SSID;
	}
}

int papermono_wifi_input_display(const struct papermono_wifi_input *input,
	char *out, size_t capacity)
{
	if (!valid(input) || out == NULL) {
		return -EINVAL;
	}
	if (capacity <= input->length) {
		if (capacity > 0U) {
			out[0] = '\0';
		}
		return -ENOSPC;
	}
	if (input->visible) {
		memmove(out, input->text, input->length);
	} else {
		memset(out, '*', input->length);
	}
	out[input->length] = '\0';
	return 0;
}

size_t papermono_wifi_input_key_count(void)
{
	return PAPERMONO_WIFI_INPUT_KEY_COUNT;
}

int papermono_wifi_input_key_at(const struct papermono_wifi_input *input,
	size_t index, struct papermono_wifi_input_key *key)
{
	if (!valid(input) || key == NULL) {
		return -EINVAL;
	}
	if (index >= PAPERMONO_WIFI_INPUT_KEY_COUNT) {
		return -ERANGE;
	}
	key->height = 60;
	key->character = '\0';
	if (index < PAPERMONO_WIFI_INPUT_CHARACTER_KEYS) {
		key->x = (int16_t)(10U + 46U * (index % 10U));
		key->y = (int16_t)(360U + 66U * (index / 10U));
		key->width = 44;
		key->action = PAPERMONO_WIFI_INPUT_CHARACTER;
		key->character = characters[input->layer][index][0];
		key->label = key->character == ' ' ? "空格" :
			characters[input->layer][index];
		return 0;
	}

	key->x = (int16_t)(10U + 92U *
		(index - PAPERMONO_WIFI_INPUT_CHARACTER_KEYS));
	key->y = 624;
	key->width = 88;
	switch (index - PAPERMONO_WIFI_INPUT_CHARACTER_KEYS) {
	case 0:
		key->action = PAPERMONO_WIFI_INPUT_SHIFT;
		key->label = input->layer == PAPERMONO_WIFI_INPUT_UPPER ?
			"小写" : "大写";
		break;
	case 1:
		key->action = PAPERMONO_WIFI_INPUT_SYMBOLS;
		key->label = input->layer == PAPERMONO_WIFI_INPUT_SYMBOL ?
			"字母" : "符号";
		break;
	case 2:
		key->action = PAPERMONO_WIFI_INPUT_BACKSPACE;
		key->label = "退格";
		break;
	case 3:
		key->action = PAPERMONO_WIFI_INPUT_CLEAR;
		key->label = "清空";
		break;
	default:
		key->action = PAPERMONO_WIFI_INPUT_VISIBILITY;
		key->label = input->visible ? "隐藏" : "显示";
		break;
	}
	return 0;
}

int papermono_wifi_input_press(struct papermono_wifi_input *input, size_t index)
{
	struct papermono_wifi_input_key key;
	int rc = papermono_wifi_input_key_at(input, index, &key);

	if (rc < 0) {
		return rc;
	}
	switch (key.action) {
	case PAPERMONO_WIFI_INPUT_CHARACTER:
		return papermono_wifi_input_append(input, key.character);
	case PAPERMONO_WIFI_INPUT_SHIFT:
		input->layer = input->layer == PAPERMONO_WIFI_INPUT_UPPER ?
			PAPERMONO_WIFI_INPUT_LOWER : PAPERMONO_WIFI_INPUT_UPPER;
		break;
	case PAPERMONO_WIFI_INPUT_SYMBOLS:
		input->layer = input->layer == PAPERMONO_WIFI_INPUT_SYMBOL ?
			PAPERMONO_WIFI_INPUT_LOWER : PAPERMONO_WIFI_INPUT_SYMBOL;
		break;
	case PAPERMONO_WIFI_INPUT_BACKSPACE:
		return papermono_wifi_input_backspace(input);
	case PAPERMONO_WIFI_INPUT_CLEAR:
		papermono_wifi_input_clear(input);
		break;
	case PAPERMONO_WIFI_INPUT_VISIBILITY:
		input->visible = !input->visible;
		break;
	}
	return 0;
}

int papermono_wifi_input_tap(struct papermono_wifi_input *input, int x, int y)
{
	if (!valid(input)) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < PAPERMONO_WIFI_INPUT_KEY_COUNT; ++index) {
		struct papermono_wifi_input_key key;
		int rc = papermono_wifi_input_key_at(input, index, &key);

		if (rc < 0) {
			return rc;
		}
		if (x >= key.x && x < key.x + key.width &&
		    y >= key.y && y < key.y + key.height) {
			rc = papermono_wifi_input_press(input, index);
			return rc < 0 ? rc : 1;
		}
	}
	return 0;
}
