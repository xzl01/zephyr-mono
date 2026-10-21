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

/**
 * Largest upload body accepted from the content center, in bytes. The HTTP
 * server cannot report the request body length in this configuration, so the
 * application reads Content-Length itself and refuses anything above this
 * before a byte of the body is stored.
 */
#define PAPERMONO_HTTP_UPLOAD_LIMIT (64U * 1024U * 1024U)

/**
 * Parse a Content-Length header value.
 *
 * Accepts 1..10 decimal digits with no sign, whitespace, or padding. Returns 0
 * and stores the value in @p length on success, -EINVAL for a malformed value,
 * and -ERANGE when the value exceeds PAPERMONO_HTTP_UPLOAD_LIMIT. A value of
 * zero is accepted here; callers decide whether an empty body is meaningful.
 */
int papermono_http_parse_content_length(const char *text, size_t *length);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_HTTP_CODEC_H_ */
