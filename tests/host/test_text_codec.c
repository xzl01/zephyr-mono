/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <papermono/text_codec.h>

static void test_utf8(void)
{
	static const uint8_t valid[] = "中文 UTF-8";
	static const uint8_t invalid_overlong[] = { 0xC0U, 0xAFU };
	static const uint8_t truncated[] = { 0xE4U, 0xB8U };
	struct papermono_decoded_codepoint decoded;

	assert(papermono_text_utf8_valid(valid, sizeof(valid) - 1U, false));
	assert(!papermono_text_utf8_valid(invalid_overlong,
		sizeof(invalid_overlong), false));
	assert(!papermono_text_utf8_valid(truncated, sizeof(truncated), false));
	assert(papermono_text_utf8_valid(truncated, sizeof(truncated), true));
	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_UTF8,
		valid, sizeof(valid) - 1U, &decoded) == 0);
	assert(decoded.codepoint == 0x4E2DU);
}

static void test_detection(void)
{
	static const uint8_t bom[] = { 0xEFU, 0xBBU, 0xBFU, 'A' };
	static const uint8_t gbk[] = { 0xD6U, 0xD0U, 0xCEU, 0xC4U };
	static const uint8_t corrupt[] = { 0x81U, 0x30U, 0xFFU };
	size_t bom_length = 0U;

	assert(papermono_text_detect(bom, sizeof(bom), &bom_length) ==
		PAPERMONO_TEXT_ENCODING_UTF8);
	assert(bom_length == 3U);
	assert(papermono_text_detect(gbk, sizeof(gbk), NULL) ==
		PAPERMONO_TEXT_ENCODING_GBK);
	assert(papermono_text_detect(corrupt, sizeof(corrupt), NULL) ==
		PAPERMONO_TEXT_ENCODING_UNKNOWN);
}

static void test_gbk(void)
{
	static const uint8_t gbk[] = { 0xD6U, 0xD0U, 0xCEU, 0xC4U };
	struct papermono_decoded_codepoint decoded;
	char utf8[4];

	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		gbk, sizeof(gbk), &decoded) == 0);
	assert(decoded.codepoint == 0x4E2DU);
	assert(decoded.consumed == 2U);
	assert(papermono_text_encode_utf8(decoded.codepoint, utf8) == 3U);
	assert(memcmp(utf8, "中", 3U) == 0);
	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		gbk + 2U, 2U, &decoded) == 0);
	assert(decoded.codepoint == 0x6587U);
}

/* Regression: an undecodable GBK pair must still consume its two bytes.
 * Advancing by one byte re-synchronises on the trail byte and corrupts every
 * following character, so the decoder substitutes U+FFFD and keeps the stride.
 */
static void test_gbk_invalid_stride(void)
{
	static const uint8_t invalid_pair[] = { 0x81U, 0x20U };
	static const uint8_t beyond_bmp[] = { 0xA1U, 0x30U };
	struct papermono_decoded_codepoint decoded;

	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		invalid_pair, sizeof(invalid_pair), &decoded) == 0);
	assert(decoded.codepoint == 0xFFFDU);
	assert(decoded.consumed == 2U);

	/* Whatever the sentinel maps to, the two-byte stride is preserved. */
	if (papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
			beyond_bmp, sizeof(beyond_bmp), &decoded) == 0) {
		assert(decoded.consumed == 2U);
	}
}

static void test_gbk_no_cascade(void)
{
	/* Corrupt pair followed by two valid characters: the valid characters
	 * must still decode, which only holds if the stride stayed even.
	 */
	static const uint8_t mixed[] = { 0x81U, 0x20U, 0xD6U, 0xD0U,
		0xCEU, 0xC4U };
	struct papermono_decoded_codepoint decoded;
	size_t offset = 0U;

	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		mixed + offset, sizeof(mixed) - offset, &decoded) == 0);
	assert(decoded.codepoint == 0xFFFDU);
	offset += decoded.consumed;

	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		mixed + offset, sizeof(mixed) - offset, &decoded) == 0);
	assert(decoded.codepoint == 0x4E2DU);
	offset += decoded.consumed;

	assert(papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_GBK,
		mixed + offset, sizeof(mixed) - offset, &decoded) == 0);
	assert(decoded.codepoint == 0x6587U);
	offset += decoded.consumed;
	assert(offset == sizeof(mixed));
}

int main(void)
{
	test_utf8();
	test_detection();
	test_gbk();
	test_gbk_invalid_stride();
	test_gbk_no_cascade();
	puts("text codec tests: PASS");
	return 0;
}
