/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_TEXT_CODEC_H_
#define PAPERMONO_TEXT_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_text_encoding {
	PAPERMONO_TEXT_ENCODING_AUTO,
	PAPERMONO_TEXT_ENCODING_UTF8,
	PAPERMONO_TEXT_ENCODING_GBK,
	PAPERMONO_TEXT_ENCODING_UNKNOWN,
};

struct papermono_decoded_codepoint {
	uint32_t codepoint;
	size_t consumed;
};

/* GBK never maps a pair to this lookup-table sentinel. The decoder can keep
 * the two-byte stride while substituting U+FFFD for an invalid pair.
 */
#define PAPERMONO_GBK_INVALID 0xFFFFU

bool papermono_text_utf8_valid(const uint8_t *data, size_t length,
	bool allow_truncated_tail);
enum papermono_text_encoding papermono_text_detect(const uint8_t *data,
	size_t length, size_t *bom_length);
int papermono_text_decode_one(enum papermono_text_encoding encoding,
	const uint8_t *data, size_t length,
	struct papermono_decoded_codepoint *decoded);
size_t papermono_text_encode_utf8(uint32_t codepoint, char output[4]);
const char *papermono_text_encoding_name(enum papermono_text_encoding encoding);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_TEXT_CODEC_H_ */
