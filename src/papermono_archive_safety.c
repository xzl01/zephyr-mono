/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <papermono/archive_safety.h>
#include <papermono/text_codec.h>
#include <papermono/zip_reader.h>

#define ZIP_CENTRAL_SIZE_LIMIT (1024U * 1024U)
#define ZIP_MAX_NESTING 16U

bool papermono_zip_safe_path(const char *path)
{
	size_t length;
	uint8_t depth = 1U;
	const char *segment = path;

	if (path == NULL) {
		return false;
	}
	length = strnlen(path, PAPERMONO_ZIP_NAME_MAX + 1U);
	if (length == 0U || length > PAPERMONO_ZIP_NAME_MAX || path[0] == '/' ||
	    path[0] == '\\' || strchr(path, ':') != NULL ||
	    strchr(path, '\\') != NULL ||
	    !papermono_text_utf8_valid((const uint8_t *)path, length, false)) {
		return false;
	}
	for (size_t index = 0U; index <= length; ++index) {
		if (path[index] != '/' && path[index] != '\0') {
			continue;
		}
		const size_t segment_length = &path[index] - segment;

		if ((segment_length == 0U && path[index] != '\0') ||
		    (segment_length == 1U && segment[0] == '.') ||
		    (segment_length == 2U && segment[0] == '.' &&
		     segment[1] == '.')) {
			return false;
		}
		if (path[index] == '/') {
			if (++depth > ZIP_MAX_NESTING) {
				return false;
			}
			segment = &path[index + 1U];
		}
	}
	return true;
}

int papermono_archive_validate_limits(uint32_t entry_count,
	uint32_t central_size, uint64_t expanded_total)
{
	return entry_count == 0U || entry_count > PAPERMONO_ZIP_ENTRY_LIMIT ||
		central_size > ZIP_CENTRAL_SIZE_LIMIT ||
		expanded_total > PAPERMONO_ZIP_TOTAL_UNCOMPRESSED_LIMIT ?
		-EFBIG : 0;
}
