/*
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include <papermono/fs_read.h>

/*
 * The ESP32-S3 SDHC/FATFS path cannot read more than its 512-byte window
 * directly into PSRAM: the transfer fails with -EIO. Large content buffers
 * (metadata parsing, ZIP directories, chapter text) live in PSRAM to keep
 * internal DRAM free, so every such read is bounced through this small
 * internal-RAM buffer instead.
 */
#define BOUNCE_SIZE 512U

static uint8_t bounce[BOUNCE_SIZE];
static K_MUTEX_DEFINE(fs_read_lock);

ssize_t papermono_fs_read_chunked(struct fs_file_t *file, void *buffer,
				  size_t length)
{
	uint8_t *out = buffer;
	size_t done = 0U;
	ssize_t result;

	if (file == NULL || buffer == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&fs_read_lock, K_FOREVER);
	while (done < length) {
		size_t chunk = MIN(length - done, sizeof(bounce));

		result = fs_read(file, bounce, chunk);
		if (result < 0) {
			k_mutex_unlock(&fs_read_lock);
			return result;
		}
		if (result == 0) {
			break;
		}
		memcpy(out + done, bounce, (size_t)result);
		done += (size_t)result;
		if ((size_t)result < chunk) {
			break;
		}
	}
	k_mutex_unlock(&fs_read_lock);
	return (ssize_t)done;
}
