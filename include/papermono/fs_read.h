/*
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PAPERMONO_FS_READ_H
#define PAPERMONO_FS_READ_H

#include <stddef.h>
#include <sys/types.h>

#include <zephyr/fs/fs.h>

/**
 * Read up to @p length bytes, bouncing through internal RAM.
 *
 * Needed because the ESP32-S3 SDHC path fails with -EIO when a multi-sector
 * read targets a PSRAM buffer. Semantics match fs_read() for a single call:
 * returns the number of bytes read (which may be short at end of file) or a
 * negative errno.
 */
ssize_t papermono_fs_read_chunked(struct fs_file_t *file, void *buffer,
				  size_t length);

#endif /* PAPERMONO_FS_READ_H */
