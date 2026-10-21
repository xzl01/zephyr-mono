/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Regression: reading a ZIP while the medium disappears must not close an
 * uninitialised struct fs_file_t.
 *
 * papermono_content_store_open() returns before fs_file_t_init() when the TF
 * card was removed (-ENODEV) or the entry is invalid (-EINVAL). Both ZIP
 * helpers close the handle unconditionally, so they must initialise it first;
 * Zephyr's fs_close() dereferences file->mp on the assumption that it is
 * either a live mount or NULL.
 *
 * Compiled with -ftrivial-auto-var-init=pattern by tests/host/run.sh so a
 * removed initialisation shows up as a non-sentinel slot instead of a
 * silently random stack value. The fake fs_close() reports that contract
 * violation rather than crashing the test binary.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <papermono/content_store.h>
#include <papermono/fs_read.h>
#include <papermono/zip_reader.h>

#include "../../src/papermono_zip_reader.c"

static int open_result = -ENODEV;
static unsigned int close_calls;
static unsigned int uninitialised_closes;
static unsigned int seek_calls;

int fs_open(struct fs_file_t *file, const char *path, int flags)
{
	(void)path;
	(void)flags;
	if (open_result < 0) {
		/* Mirrors the production failure: the handle is left untouched. */
		return open_result;
	}
	fs_file_t_init(file);
	file->slot = 1;
	return 0;
}

int fs_close(struct fs_file_t *file)
{
	++close_calls;
	/* fs_file_t_init() leaves slot == -1; a live handle uses >= 0. */
	if (file->slot != -1) {
		++uninitialised_closes;
	}
	return 0;
}

int fs_seek(struct fs_file_t *file, off_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	++seek_calls;
	return 0;
}

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	(void)file;
	(void)buffer;
	(void)length;
	return 0;
}

ssize_t papermono_fs_read_chunked(struct fs_file_t *file, void *buffer,
	size_t length)
{
	(void)file;
	(void)buffer;
	(void)length;
	return -EIO;
}

int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file)
{
	(void)entry;
	assert(file != NULL);
	return fs_open(file, "unused", 0);
}

void tinfl_init(tinfl_decompressor *decompressor)
{
	(void)decompressor;
}

tinfl_status tinfl_decompress(tinfl_decompressor *decompressor,
	const uint8_t *input, size_t *input_length, uint8_t *output,
	uint8_t *output_end, size_t *output_length, uint32_t flags)
{
	(void)decompressor;
	(void)input;
	(void)input_length;
	(void)output;
	(void)output_end;
	(void)output_length;
	(void)flags;
	return TINFL_STATUS_FAILED;
}

static void reset_counters(void)
{
	close_calls = 0U;
	uninitialised_closes = 0U;
	seek_calls = 0U;
}

static void check_missing_medium(void)
{
	struct papermono_zip_archive archive;
	struct papermono_zip_entry entry;

	memset(&archive, 0, sizeof(archive));
	memset(&entry, 0, sizeof(entry));

	/* Card removed / polling failed: open returns -ENODEV untouched. */
	open_result = -ENODEV;
	reset_counters();
	assert(papermono_zip_find(&archive, "OEBPS/chapter.xhtml", &entry) ==
		-ENODEV);
	assert(close_calls == 1U);
	assert(uninitialised_closes == 0U);
	assert(seek_calls == 0U);

	open_result = -ENODEV;
	reset_counters();
	(void)strcpy(entry.name, "OEBPS/chapter.xhtml");
	entry.uncompressed_size = 16U;
	assert(papermono_zip_extract(&archive, &entry, NULL, NULL) == -EINVAL);
	assert(close_calls == 0U);

	/* Callback NULL is rejected before any open/close. */
	reset_counters();
	assert(papermono_zip_extract(&archive, &entry, (papermono_zip_write_cb)1,
		NULL) == -ENODEV);
	assert(close_calls == 1U);
	assert(uninitialised_closes == 0U);
}

int main(void)
{
	check_missing_medium();
	puts("zip open-failure handle initialisation: PASS");
	return 0;
}
