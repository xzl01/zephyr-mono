/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <papermono/archive_safety.h>
#include <papermono/zip_reader.h>

int main(void)
{
	assert(papermono_zip_safe_path("OPS/chapter.xhtml"));
	assert(papermono_zip_safe_path("中文/章节.xhtml"));
	assert(!papermono_zip_safe_path("../escape"));
	assert(!papermono_zip_safe_path("OPS/../escape"));
	assert(!papermono_zip_safe_path("/absolute"));
	assert(!papermono_zip_safe_path("C:drive"));
	assert(!papermono_zip_safe_path("OPS//empty"));
	assert(!papermono_zip_safe_path(
		"a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/file.xhtml"));
	char overlong[PAPERMONO_ZIP_NAME_MAX + 2U];
	memset(overlong, 'a', sizeof(overlong));
	overlong[sizeof(overlong) - 1U] = '\0';
	assert(!papermono_zip_safe_path(overlong));
	assert(papermono_archive_validate_limits(1U, 1024U, 4096U) == 0);
	assert(papermono_archive_validate_limits(0U, 0U, 0U) == -EFBIG);
	assert(papermono_archive_validate_limits(
		PAPERMONO_ZIP_ENTRY_LIMIT + 1U, 0U, 0U) == -EFBIG);
	assert(papermono_archive_validate_limits(1U, 2U * 1024U * 1024U,
		0U) == -EFBIG);
	assert(papermono_archive_validate_limits(1U, 1024U,
		PAPERMONO_ZIP_TOTAL_UNCOMPRESSED_LIMIT + 1ULL) == -EFBIG);
	puts("archive safety tests: PASS");
	return 0;
}
