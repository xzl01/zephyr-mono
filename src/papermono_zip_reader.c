/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <miniz.h>

#include <papermono/fs_read.h>
#include <papermono/text_codec.h>
#include <papermono/archive_safety.h>
#include <papermono/zip_reader.h>

LOG_MODULE_REGISTER(papermono_zip, LOG_LEVEL_INF);

#define ZIP_EOCD_SIGNATURE UINT32_C(0x06054B50)
#define ZIP_CENTRAL_SIGNATURE UINT32_C(0x02014B50)
#define ZIP_LOCAL_SIGNATURE UINT32_C(0x04034B50)
#define ZIP_EOCD_MAX_SEARCH (65535U + 22U)
#define ZIP_IO_BUFFER_SIZE 4096U

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_ZIP_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_ZIP_BSS
#endif

static uint8_t eocd_buffer[ZIP_EOCD_MAX_SEARCH] PAPERMONO_ZIP_BSS;
static uint8_t input_buffer[ZIP_IO_BUFFER_SIZE] PAPERMONO_ZIP_BSS;
static uint8_t dictionary[TINFL_LZ_DICT_SIZE] PAPERMONO_ZIP_BSS;
static tinfl_decompressor inflator PAPERMONO_ZIP_BSS;
K_MUTEX_DEFINE(zip_lock);

static uint16_t read_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int open_source(const struct papermono_zip_archive *archive,
	struct fs_file_t *file)
{
	return papermono_content_store_open(&archive->source, file);
}

static int read_exact(struct fs_file_t *file, void *buffer, size_t length)
{
	uint8_t *output = buffer;
	size_t received_total = 0U;

	while (received_total < length) {
		ssize_t received = papermono_fs_read_chunked(file, output + received_total,
			length - received_total);

		if (received < 0) {
			return (int)received;
		}
		if (received == 0) {
			return -ENODATA;
		}
		received_total += (size_t)received;
	}
	return 0;
}

/* Read one central-directory record and always advance past it.
 *
 * Returns 0 for a usable entry. Returns -ENOTSUP (ZIP64, encrypted, an
 * unsupported method, or an oversized entry), -EPERM (unsafe name), or
 * -ENAMETOOLONG (name too long) *after* consuming the record: those mean
 * "this entry is unusable, but the rest of the archive is still readable".
 * Any other negative value is an I/O or framing error and the archive cannot
 * be trusted, because the cursor position is no longer meaningful.
 *
 * Treating an entry-level rejection as fatal used to make a whole book
 * unopenable over one stray entry, which is common in real EPUBs (an encoder
 * writing GBK chapter names, a cover image above the per-entry cap).
 */
static int read_central_entry(struct fs_file_t *file,
	struct papermono_zip_entry *entry)
{
	uint8_t header[46];
	uint16_t name_length;
	uint16_t extra_length;
	uint16_t comment_length;
	int ret = read_exact(file, header, sizeof(header));

	if (ret < 0) {
		return ret;
	}
	if (read_le32(header) != ZIP_CENTRAL_SIGNATURE) {
		return -EBADMSG;
	}
	name_length = read_le16(header + 28U);
	extra_length = read_le16(header + 30U);
	comment_length = read_le16(header + 32U);
	if (name_length == 0U || name_length > PAPERMONO_ZIP_NAME_MAX) {
		/* Still consume the variable-length tail so the caller can move on
		 * to the next record.
		 */
		ret = fs_seek(file,
			(off_t)name_length + extra_length + comment_length,
			FS_SEEK_CUR);
		return ret < 0 ? ret : -ENAMETOOLONG;
	}
	memset(entry, 0, sizeof(*entry));
	entry->flags = read_le16(header + 8U);
	entry->method = read_le16(header + 10U);
	entry->crc32 = read_le32(header + 16U);
	entry->compressed_size = read_le32(header + 20U);
	entry->uncompressed_size = read_le32(header + 24U);
	entry->local_header_offset = read_le32(header + 42U);
	const bool unsupported =
		entry->compressed_size == UINT32_MAX ||
		entry->uncompressed_size == UINT32_MAX ||
		entry->local_header_offset == UINT32_MAX ||
		(entry->flags & 1U) != 0U ||
		(entry->method != 0U && entry->method != 8U) ||
		entry->uncompressed_size > PAPERMONO_ZIP_ENTRY_UNCOMPRESSED_LIMIT;

	ret = read_exact(file, entry->name, name_length);
	if (ret < 0) {
		return ret;
	}
	entry->name[name_length] = '\0';
	ret = fs_seek(file, (off_t)extra_length + comment_length, FS_SEEK_CUR);
	if (ret < 0) {
		return ret;
	}
	if (unsupported) {
		return -ENOTSUP;
	}
	return papermono_zip_safe_path(entry->name) ? 0 : -EPERM;
}

int papermono_zip_open(struct papermono_zip_archive *archive,
	const struct papermono_content_entry *source)
{
	struct fs_file_t file;
	uint32_t search_length;
	uint32_t search_start;
	uint32_t eocd_offset = UINT32_MAX;
	uint64_t expanded_total = 0U;
	int ret;

	if (archive == NULL || source == NULL ||
	    source->kind != PAPERMONO_CONTENT_EPUB || source->size < 22U ||
	    source->size > PAPERMONO_BOOK_FILE_LIMIT) {
		return -EINVAL;
	}
	memset(archive, 0, sizeof(*archive));
	archive->source = *source;
	search_length = (uint32_t)MIN(source->size, ZIP_EOCD_MAX_SEARCH);
	search_start = (uint32_t)source->size - search_length;
	k_mutex_lock(&zip_lock, K_FOREVER);
	ret = papermono_content_store_open(source, &file);
	if (ret < 0) {
		goto out;
	}
	ret = fs_seek(&file, search_start, FS_SEEK_SET);
	ret = ret ?: read_exact(&file, eocd_buffer, search_length);
	if (ret < 0) {
		goto close;
	}
	for (int32_t offset = (int32_t)search_length - 22; offset >= 0;
	     --offset) {
		if (read_le32(&eocd_buffer[offset]) == ZIP_EOCD_SIGNATURE &&
		    offset + 22 + read_le16(&eocd_buffer[offset + 20U]) ==
			(int32_t)search_length) {
			eocd_offset = (uint32_t)offset;
			break;
		}
	}
	if (eocd_offset == UINT32_MAX) {
		ret = -EBADMSG;
		goto close;
	}
	const uint8_t *eocd = &eocd_buffer[eocd_offset];
	if (read_le16(eocd + 4U) != 0U || read_le16(eocd + 6U) != 0U ||
	    read_le16(eocd + 8U) != read_le16(eocd + 10U)) {
		ret = -ENOTSUP;
		goto close;
	}
	archive->entry_count = read_le16(eocd + 10U);
	archive->central_size = read_le32(eocd + 12U);
	archive->central_offset = read_le32(eocd + 16U);
	if (papermono_archive_validate_limits(archive->entry_count,
		archive->central_size, 0U) < 0 ||
	    (uint64_t)archive->central_offset + archive->central_size >
		source->size) {
		ret = -EFBIG;
		goto close;
	}
	ret = fs_seek(&file, archive->central_offset, FS_SEEK_SET);
	for (uint16_t index = 0U; ret == 0 && index < archive->entry_count;
	     ++index) {
		struct papermono_zip_entry entry;
		int entry_ret = read_central_entry(&file, &entry);

		if (entry_ret == -ENOTSUP || entry_ret == -EPERM ||
		    entry_ret == -ENAMETOOLONG) {
			/* This one entry cannot be used, but the archive is still
			 * addressable: skip it rather than reject the book.
			 */
			continue;
		}
		if (entry_ret < 0) {
			ret = entry_ret;
			break;
		}
		expanded_total += entry.uncompressed_size;
		if (papermono_archive_validate_limits(
			archive->entry_count, archive->central_size,
			expanded_total) < 0 ||
		    (uint64_t)entry.local_header_offset + 30U +
			entry.compressed_size > source->size) {
			ret = -EFBIG;
		}
	}
close:
	(void)fs_close(&file);
out:
	k_mutex_unlock(&zip_lock);
	return ret;
}

int papermono_zip_find(const struct papermono_zip_archive *archive,
	const char *name, struct papermono_zip_entry *entry)
{
	struct fs_file_t file;
	int ret;

	if (archive == NULL || name == NULL || entry == NULL ||
	    !papermono_zip_safe_path(name)) {
		return -EINVAL;
	}
	k_mutex_lock(&zip_lock, K_FOREVER);
	ret = open_source(archive, &file);
	ret = ret ?: fs_seek(&file, archive->central_offset, FS_SEEK_SET);
	for (uint16_t index = 0U; ret == 0 && index < archive->entry_count;
	     ++index) {
		int entry_ret = read_central_entry(&file, entry);

		if (entry_ret == -ENOTSUP || entry_ret == -EPERM) {
			/* The record was consumed, so keep scanning. Report the
			 * per-entry reason only if this is the name that was
			 * actually requested; entry->name is populated for both of
			 * these statuses.
			 */
			if (strcmp(entry->name, name) == 0) {
				(void)fs_close(&file);
				k_mutex_unlock(&zip_lock);
				return entry_ret;
			}
			continue;
		}
		if (entry_ret == -ENAMETOOLONG) {
			/* The name was too long to be stored, so it cannot be the
			 * requested one; keep scanning.
			 */
			continue;
		}
		if (entry_ret < 0) {
			ret = entry_ret;
			break;
		}
		if (strcmp(entry->name, name) == 0) {
			(void)fs_close(&file);
			k_mutex_unlock(&zip_lock);
			return 0;
		}
	}
	(void)fs_close(&file);
	k_mutex_unlock(&zip_lock);
	return ret < 0 ? ret : -ENOENT;
}

static int deliver(papermono_zip_write_cb callback, void *context,
	const uint8_t *data, size_t length, uint32_t *crc, uint32_t *total)
{
	int ret;

	if ((uint64_t)*total + length > PAPERMONO_ZIP_ENTRY_UNCOMPRESSED_LIMIT) {
		return -EFBIG;
	}
	*crc = crc32_ieee_update(*crc, data, length);
	*total += (uint32_t)length;
	ret = callback(data, length, context);
	return ret == 0 ? 0 : PAPERMONO_ZIP_EXTRACT_STOPPED;
}

static int extract_stored(struct fs_file_t *file,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context, uint32_t *crc, uint32_t *total)
{
	uint32_t remaining = entry->compressed_size;

	while (remaining > 0U) {
		const size_t chunk = MIN(remaining, sizeof(input_buffer));
		int ret = read_exact(file, input_buffer, chunk);

		if (ret < 0) {
			return ret;
		}
		ret = deliver(callback, context, input_buffer, chunk, crc, total);
		if (ret != 0) {
			return ret;
		}
		remaining -= chunk;
	}
	return 0;
}

static int extract_deflated(struct fs_file_t *file,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context, uint32_t *crc, uint32_t *total)
{
	uint32_t compressed_remaining = entry->compressed_size;
	size_t input_position = 0U;
	size_t input_available = 0U;
	size_t output_position = 0U;
	tinfl_status status;

	tinfl_init(&inflator);
	for (;;) {
		if (input_position == input_available && compressed_remaining > 0U) {
			input_available = MIN(compressed_remaining,
				(uint32_t)sizeof(input_buffer));
			int ret = read_exact(file, input_buffer, input_available);

			if (ret < 0) {
				return ret;
			}
			compressed_remaining -= input_available;
			input_position = 0U;
		}
		size_t input_size = input_available - input_position;
		size_t output_size = sizeof(dictionary) - output_position;
		const uint32_t flags = compressed_remaining > 0U ?
			TINFL_FLAG_HAS_MORE_INPUT : 0U;

		status = tinfl_decompress(&inflator, input_buffer + input_position,
			&input_size, dictionary, dictionary + output_position,
			&output_size, flags);
		input_position += input_size;
		if (output_size > 0U) {
			int ret = deliver(callback, context,
				dictionary + output_position, output_size, crc, total);

			if (ret != 0) {
				return ret;
			}
			output_position =
				(output_position + output_size) & (TINFL_LZ_DICT_SIZE - 1U);
		}
		if (status == TINFL_STATUS_DONE) {
			return 0;
		}
		if (status < 0 || (status == TINFL_STATUS_NEEDS_MORE_INPUT &&
		    compressed_remaining == 0U && input_position == input_available)) {
			return -EBADMSG;
		}
	}
}

int papermono_zip_extract(const struct papermono_zip_archive *archive,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context)
{
	struct fs_file_t file;
	uint8_t local_header[30];
	uint32_t crc = 0U;
	uint32_t total = 0U;
	uint16_t name_length;
	uint16_t extra_length;
	int ret;

	if (archive == NULL || entry == NULL || callback == NULL ||
	    !papermono_zip_safe_path(entry->name) ||
	    entry->uncompressed_size > PAPERMONO_ZIP_ENTRY_UNCOMPRESSED_LIMIT) {
		return -EINVAL;
	}
	k_mutex_lock(&zip_lock, K_FOREVER);
	ret = open_source(archive, &file);
	ret = ret ?: fs_seek(&file, entry->local_header_offset, FS_SEEK_SET);
	ret = ret ?: read_exact(&file, local_header, sizeof(local_header));
	if (ret < 0 || read_le32(local_header) != ZIP_LOCAL_SIGNATURE) {
		ret = ret < 0 ? ret : -EBADMSG;
		goto close;
	}
	name_length = read_le16(local_header + 26U);
	extra_length = read_le16(local_header + 28U);
	ret = fs_seek(&file, (off_t)name_length + extra_length, FS_SEEK_CUR);
	if (ret < 0) {
		goto close;
	}
	ret = entry->method == 0U ?
		extract_stored(&file, entry, callback, context, &crc, &total) :
		extract_deflated(&file, entry, callback, context, &crc, &total);
	if (ret == 0 && (total != entry->uncompressed_size ||
	    crc != entry->crc32)) {
		ret = -EBADMSG;
	}
close:
	(void)fs_close(&file);
	k_mutex_unlock(&zip_lock);
	return ret;
}
