/*
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/fs/fs.h>

#include <papermono/fs_read.h>

/*
 * Simulate the ESP32-S3 SDHC limitation: a single read that targets a PSRAM
 * buffer and spans more than one 512-byte window fails with -EIO. The helper
 * must never issue such a request, so large logical reads still succeed.
 */
#define WINDOW 512U

struct fake_file {
	struct fs_file_t file;
	size_t offset;
	size_t length;
};

static struct fake_file fake;
static uint8_t source[4096];
static uint8_t destination[4096];
static size_t largest_request;
static size_t calls;

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	struct fake_file *state = (struct fake_file *)file;

	if (length > WINDOW) {
		largest_request = length;
		return -EIO; /* would be a multi-window PSRAM transfer */
	}
	largest_request = length > largest_request ? length : largest_request;
	++calls;
	if (state->offset >= state->length) {
		return 0;
	}
	if (length > state->length - state->offset) {
		length = state->length - state->offset;
	}
	memcpy(buffer, source + state->offset, length);
	state->offset += length;
	return (ssize_t)length;
}

int main(void)
{
	memset(&fake, 0, sizeof(fake));
	for (size_t i = 0; i < sizeof(source); ++i) {
		source[i] = (uint8_t)(i * 7U + 3U);
	}

	/* 4 KiB read must be split into window-sized requests. */
	fake.length = sizeof(source);
	memset(destination, 0, sizeof(destination));
	assert(papermono_fs_read_chunked(&fake.file, destination, sizeof(source)) ==
		(ssize_t)sizeof(source));
	assert(memcmp(destination, source, sizeof(source)) == 0);
	assert(largest_request <= WINDOW);
	assert(calls >= sizeof(source) / WINDOW);

	/* Short file: one short read at EOF. */
	memset(&fake, 0, sizeof(fake));
	fake.length = 700;
	memset(destination, 0, sizeof(destination));
	assert(papermono_fs_read_chunked(&fake.file, destination, 1024) == 700);
	assert(memcmp(destination, source, 700) == 0);

	/* Empty file reads zero bytes rather than failing. */
	memset(&fake, 0, sizeof(fake));
	fake.length = 0;
	assert(papermono_fs_read_chunked(&fake.file, destination, 128) == 0);

	/* Argument validation. */
	assert(papermono_fs_read_chunked(NULL, destination, 16) == -EINVAL);
	assert(papermono_fs_read_chunked(&fake.file, NULL, 16) == -EINVAL);

	puts("bounded filesystem reads: PASS");
	return 0;
}
