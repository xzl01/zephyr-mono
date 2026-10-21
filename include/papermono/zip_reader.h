/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_ZIP_READER_H_
#define PAPERMONO_ZIP_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <papermono/content_store.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_ZIP_NAME_MAX 255U
#define PAPERMONO_ZIP_ENTRY_LIMIT 256U
#define PAPERMONO_ZIP_ENTRY_UNCOMPRESSED_LIMIT (16U * 1024U * 1024U)
#define PAPERMONO_ZIP_TOTAL_UNCOMPRESSED_LIMIT (128U * 1024U * 1024U)
#define PAPERMONO_ZIP_EXTRACT_STOPPED 1

struct papermono_zip_archive {
	struct papermono_content_entry source;
	uint32_t central_offset;
	uint32_t central_size;
	uint16_t entry_count;
};

struct papermono_zip_entry {
	char name[PAPERMONO_ZIP_NAME_MAX + 1U];
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t local_header_offset;
	uint16_t method;
	uint16_t flags;
};

typedef int (*papermono_zip_write_cb)(const uint8_t *data, size_t length,
	void *context);

int papermono_zip_open(struct papermono_zip_archive *archive,
	const struct papermono_content_entry *source);
int papermono_zip_find(const struct papermono_zip_archive *archive,
	const char *name, struct papermono_zip_entry *entry);
int papermono_zip_extract(const struct papermono_zip_archive *archive,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context);
bool papermono_zip_safe_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ZIP_READER_H_ */
