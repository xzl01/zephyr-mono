/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <papermono/http_codec.h>

static int hex_digit(char value)
{
	return value >= '0' && value <= '9' ? value - '0' :
		value >= 'a' && value <= 'f' ? value - 'a' + 10 :
		value >= 'A' && value <= 'F' ? value - 'A' + 10 : -1;
}

static int base64url_digit(char value)
{
	return value >= 'A' && value <= 'Z' ? value - 'A' :
		value >= 'a' && value <= 'z' ? value - 'a' + 26 :
		value >= '0' && value <= '9' ? value - '0' + 52 :
		value == '-' ? 62 : value == '_' ? 63 : -1;
}

static int decode_base64url(const char *encoded, char *name, size_t capacity)
{
	uint32_t accumulator = 0U;
	uint8_t bits = 0U;
	size_t output = 0U;
	const size_t length = strlen(encoded);

	if (length == 0U || length % 4U == 1U) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < length; ++index) {
		const int digit = base64url_digit(encoded[index]);

		if (digit < 0) {
			return -EINVAL;
		}
		accumulator = (accumulator << 6U) | (uint32_t)digit;
		bits += 6U;
		if (bits >= 8U) {
			bits -= 8U;
			const uint8_t value = (uint8_t)(accumulator >> bits);

			if (value == 0U) {
				return -EINVAL;
			}
			if (output + 1U >= capacity) {
				return -ENAMETOOLONG;
			}
			name[output++] = (char)value;
		}
	}
	if (bits > 0U &&
	    (accumulator & ((1U << bits) - 1U)) != 0U) {
		return -EINVAL;
	}
	name[output] = '\0';
	return 0;
}

int papermono_http_decode_name(const char *encoded, char *name,
	size_t capacity)
{
	size_t output = 0U;

	if (encoded == NULL || name == NULL || capacity == 0U) {
		return -EINVAL;
	}
	if (strncmp(encoded, "b64:", 4U) == 0) {
		return decode_base64url(encoded + 4U, name, capacity);
	}
	for (size_t index = 0U; encoded[index] != '\0'; ++index) {
		unsigned int value = (unsigned char)encoded[index];

		if (encoded[index] == '%') {
			const int high = encoded[index + 1U] == '\0' ? -1 :
				hex_digit(encoded[index + 1U]);
			const int low = encoded[index + 1U] == '\0' ||
				encoded[index + 2U] == '\0' ? -1 :
				hex_digit(encoded[index + 2U]);

			if (high < 0 || low < 0) {
				return -EINVAL;
			}
			value = (unsigned int)((high << 4) | low);
			index += 2U;
		}
		if (value == 0U) {
			return -EINVAL;
		}
		if (output + 1U >= capacity) {
			return -ENAMETOOLONG;
		}
		name[output++] = (char)value;
	}
	name[output] = '\0';
	return 0;
}

bool papermono_http_parse_crc32(const char *text, uint32_t *crc)
{
	size_t length;
	uint32_t value = 0U;

	if (text == NULL || crc == NULL) {
		return false;
	}
	length = strlen(text);
	if (length == 0U || length > 8U) {
		return false;
	}
	for (size_t index = 0U; index < length; ++index) {
		const int digit = hex_digit(text[index]);

		if (digit < 0) {
			return false;
		}
		value = (value << 4U) | (uint32_t)digit;
	}
	*crc = value;
	return true;
}

#define CONTENT_LENGTH_DIGITS_MAX 10U

int papermono_http_parse_content_length(const char *text, size_t *length)
{
	size_t value = 0U;
	size_t digits = 0U;

	if (text == NULL || length == NULL) {
		return -EINVAL;
	}
	/* RFC 9110 allows leading zeros, and the limit check below is what keeps
	 * the accumulation in range, so every digit run is accepted here.
	 */
	for (const char *cursor = text; *cursor != '\0'; ++cursor) {
		if (*cursor < '0' || *cursor > '9') {
			return -EINVAL;
		}
		if (++digits > CONTENT_LENGTH_DIGITS_MAX) {
			return -ERANGE;
		}
		value = value * 10U + (size_t)(*cursor - '0');
	}
	if (digits == 0U) {
		return -EINVAL;
	}
	if (value > (size_t)PAPERMONO_HTTP_UPLOAD_LIMIT) {
		return -ERANGE;
	}
	*length = value;
	return 0;
}
