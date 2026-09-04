/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_TEXT_LAYOUT_H_
#define PAPERMONO_TEXT_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return true when a character must not be left at the start of a line. */
bool papermono_text_prohibits_line_start(uint32_t codepoint);

/** Return true when a character must not be left at the end of a line. */
bool papermono_text_prohibits_line_end(uint32_t codepoint);

/**
 * Decide whether a soft line break is required before @p codepoint.
 *
 * Chinese closing punctuation may use up to two spare margin cells so it is
 * not stranded at the start of the following line. Opening punctuation is
 * moved to the next line instead of occupying the final regular cell.
 */
bool papermono_text_should_wrap_before(uint32_t codepoint, uint8_t column,
	uint8_t max_columns);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_TEXT_LAYOUT_H_ */
