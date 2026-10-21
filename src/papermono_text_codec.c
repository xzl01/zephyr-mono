/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>

#include <papermono/text_codec.h>

uint16_t papermono_gbk_unicode(uint8_t lead, uint8_t trail);

static int decode_utf8(const uint8_t *data, size_t length,
	struct papermono_decoded_codepoint *decoded)
{
	uint32_t codepoint;
	size_t needed;
	uint32_t minimum;

	if (length == 0U) {
		return -EAGAIN;
	}
	if (data[0] < 0x80U) {
		decoded->codepoint = data[0];
		decoded->consumed = 1U;
		return 0;
	}
	if ((data[0] & 0xE0U) == 0xC0U) {
		codepoint = data[0] & 0x1FU;
		needed = 2U;
		minimum = 0x80U;
	} else if ((data[0] & 0xF0U) == 0xE0U) {
		codepoint = data[0] & 0x0FU;
		needed = 3U;
		minimum = 0x800U;
	} else if ((data[0] & 0xF8U) == 0xF0U) {
		codepoint = data[0] & 0x07U;
		needed = 4U;
		minimum = 0x10000U;
	} else {
		return -EILSEQ;
	}
	if (length < needed) {
		return -EAGAIN;
	}
	for (size_t index = 1U; index < needed; ++index) {
		if ((data[index] & 0xC0U) != 0x80U) {
			return -EILSEQ;
		}
		codepoint = (codepoint << 6) | (data[index] & 0x3FU);
	}
	if (codepoint < minimum || codepoint > 0x10FFFFU ||
	    (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
		return -EILSEQ;
	}
	decoded->codepoint = codepoint;
	decoded->consumed = needed;
	return 0;
}

bool papermono_text_utf8_valid(const uint8_t *data, size_t length,
	bool allow_truncated_tail)
{
	size_t offset = 0U;

	if (data == NULL && length != 0U) {
		return false;
	}
	while (offset < length) {
		struct papermono_decoded_codepoint decoded;
		int ret = decode_utf8(data + offset, length - offset, &decoded);

		if (ret == -EAGAIN && allow_truncated_tail) {
			return true;
		}
		if (ret < 0) {
			return false;
		}
		offset += decoded.consumed;
	}
	return true;
}

static bool gbk_valid(const uint8_t *data, size_t length)
{
	size_t offset = 0U;

	while (offset < length) {
		if (data[offset] < 0x80U) {
			++offset;
			continue;
		}
		if (offset + 1U >= length) {
			return true;
		}
		if (papermono_gbk_unicode(data[offset], data[offset + 1U]) ==
		    PAPERMONO_GBK_INVALID) {
			return false;
		}
		offset += 2U;
	}
	return true;
}

enum papermono_text_encoding papermono_text_detect(const uint8_t *data,
	size_t length, size_t *bom_length)
{
	if (bom_length != NULL) {
		*bom_length = 0U;
	}
	if (data == NULL && length != 0U) {
		return PAPERMONO_TEXT_ENCODING_UNKNOWN;
	}
	if (length >= 3U && data[0] == 0xEFU && data[1] == 0xBBU &&
	    data[2] == 0xBFU) {
		if (bom_length != NULL) {
			*bom_length = 3U;
		}
		return PAPERMONO_TEXT_ENCODING_UTF8;
	}
	if (papermono_text_utf8_valid(data, length, true)) {
		return PAPERMONO_TEXT_ENCODING_UTF8;
	}
	if (gbk_valid(data, length)) {
		return PAPERMONO_TEXT_ENCODING_GBK;
	}
	return PAPERMONO_TEXT_ENCODING_UNKNOWN;
}

int papermono_text_decode_one(enum papermono_text_encoding encoding,
	const uint8_t *data, size_t length,
	struct papermono_decoded_codepoint *decoded)
{
	uint16_t codepoint;

	if (data == NULL || decoded == NULL) {
		return -EINVAL;
	}
	if (encoding == PAPERMONO_TEXT_ENCODING_UTF8) {
		return decode_utf8(data, length, decoded);
	}
	if (encoding != PAPERMONO_TEXT_ENCODING_GBK) {
		return -EINVAL;
	}
	if (length == 0U) {
		return -EAGAIN;
	}
	if (data[0] < 0x80U) {
		decoded->codepoint = data[0];
		decoded->consumed = 1U;
		return 0;
	}
	if (length < 2U) {
		return -EAGAIN;
	}
	codepoint = papermono_gbk_unicode(data[0], data[1]);

	/* GBK is a two-byte code: every outcome below consumes both bytes.
	 * Returning -EILSEQ here would let callers advance by a single byte,
	 * which re-synchronises on the trail byte and corrupts the rest of the
	 * buffer, so undecodable pairs are reported as U+FFFD instead.
	 */
	if (codepoint == PAPERMONO_GBK_INVALID) {
		decoded->codepoint = 0xFFFDU;
		decoded->consumed = 2U;
		return 0;
	}
	decoded->codepoint = codepoint;
	decoded->consumed = 2U;
	return 0;
}

size_t papermono_text_encode_utf8(uint32_t codepoint, char output[4])
{
	if (codepoint <= 0x7FU) {
		output[0] = (char)codepoint;
		return 1U;
	}
	if (codepoint <= 0x7FFU) {
		output[0] = (char)(0xC0U | (codepoint >> 6));
		output[1] = (char)(0x80U | (codepoint & 0x3FU));
		return 2U;
	}
	if (codepoint <= 0xFFFFU &&
	    !(codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
		output[0] = (char)(0xE0U | (codepoint >> 12));
		output[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
		output[2] = (char)(0x80U | (codepoint & 0x3FU));
		return 3U;
	}
	if (codepoint <= 0x10FFFFU) {
		output[0] = (char)(0xF0U | (codepoint >> 18));
		output[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
		output[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
		output[3] = (char)(0x80U | (codepoint & 0x3FU));
		return 4U;
	}
	return 0U;
}

const char *papermono_text_encoding_name(enum papermono_text_encoding encoding)
{
	switch (encoding) {
	case PAPERMONO_TEXT_ENCODING_UTF8:
		return "UTF-8";
	case PAPERMONO_TEXT_ENCODING_GBK:
		return "GBK";
	case PAPERMONO_TEXT_ENCODING_AUTO:
		return "AUTO";
	case PAPERMONO_TEXT_ENCODING_UNKNOWN:
	default:
		return "UNKNOWN";
	}
}
