/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_HTTP_CODEC_H_
#define PAPERMONO_HTTP_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_http_decode_name(const char *encoded, char *name,
	size_t capacity);
bool papermono_http_parse_crc32(const char *text, uint32_t *crc);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_HTTP_CODEC_H_ */
