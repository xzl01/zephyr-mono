/* SPDX-License-Identifier: MIT */

#include <papermono/text_layout.h>

#define PAPERMONO_TEXT_PUNCTUATION_OVERFLOW 2U

static bool codepoint_in(const uint32_t *values, uint8_t count,
	uint32_t codepoint)
{
	for (uint8_t index = 0U; index < count; ++index) {
		if (values[index] == codepoint) {
			return true;
		}
	}
	return false;
}

bool papermono_text_prohibits_line_start(uint32_t codepoint)
{
	static const uint32_t values[] = {
		'!', '%', ')', ',', '.', ':', ';', '?', ']', '}',
		0x00BBU, 0x2019U, 0x201DU, 0x2026U,
		0x3001U, 0x3002U, 0x3009U, 0x300BU, 0x300DU, 0x300FU,
		0x3011U, 0x3015U, 0x3017U, 0x3019U, 0x301BU,
		0xFF01U, 0xFF05U, 0xFF09U, 0xFF0CU, 0xFF0EU, 0xFF1AU,
		0xFF1BU, 0xFF1FU, 0xFF3DU, 0xFF5DU, 0xFF61U, 0xFF63U,
		0xFF64U,
	};

	return codepoint_in(values, (uint8_t)(sizeof(values) / sizeof(values[0])),
		codepoint);
}

bool papermono_text_prohibits_line_end(uint32_t codepoint)
{
	static const uint32_t values[] = {
		'(', '[', '{', 0x00ABU, 0x2018U, 0x201CU,
		0x3008U, 0x300AU, 0x300CU, 0x300EU, 0x3010U,
		0x3014U, 0x3016U, 0x3018U, 0x301AU,
		0xFF08U, 0xFF3BU, 0xFF5BU, 0xFF62U,
	};

	return codepoint_in(values, (uint8_t)(sizeof(values) / sizeof(values[0])),
		codepoint);
}

bool papermono_text_should_wrap_before(uint32_t codepoint, uint8_t column,
	uint8_t max_columns)
{
	if (codepoint == '\n' || column == 0U || max_columns == 0U) {
		return false;
	}
	if (papermono_text_prohibits_line_end(codepoint) &&
	    column + 1U >= max_columns) {
		return true;
	}
	if (column < max_columns) {
		return false;
	}
	if (papermono_text_prohibits_line_start(codepoint) &&
	    column < max_columns + PAPERMONO_TEXT_PUNCTUATION_OVERFLOW) {
		return false;
	}
	return true;
}
