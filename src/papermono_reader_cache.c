/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>

#include <papermono/reader_cache.h>

#define CACHE_HEADER_SIZE 48U
#define CACHE_ENTRY_SIZE 8U

static const uint8_t cache_magic[4] = { 'P', 'M', 'P', 'I' };

static uint16_t read_le16(const uint8_t *value)
{
	return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t read_le32(const uint8_t *value)
{
	return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
		(uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static uint64_t read_le64(const uint8_t *value)
{
	uint64_t result = 0U;

	for (size_t index = 0U; index < 8U; ++index) {
		result |= (uint64_t)value[index] << (index * 8U);
	}
	return result;
}

static void write_le16(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *output, uint32_t value)
{
	for (size_t index = 0U; index < 4U; ++index) {
		output[index] = (uint8_t)(value >> (index * 8U));
	}
}

static void write_le64(uint8_t *output, uint64_t value)
{
	for (size_t index = 0U; index < 8U; ++index) {
		output[index] = (uint8_t)(value >> (index * 8U));
	}
}

static uint32_t header_checksum(const uint8_t *header)
{
	uint32_t hash = UINT32_C(2166136261);

	for (size_t index = 0U; index < CACHE_HEADER_SIZE - 4U; ++index) {
		hash = (hash ^ header[index]) * UINT32_C(16777619);
	}
	return hash;
}

static void encode_header(const struct papermono_reader_cache_key *key,
	uint8_t *header)
{
	memset(header, 0, CACHE_HEADER_SIZE);
	memcpy(header, cache_magic, sizeof(cache_magic));
	header[4] = PAPERMONO_READER_CACHE_FORMAT_VERSION;
	header[5] = key->kind;
	header[6] = key->encoding;
	header[7] = key->density;
	header[8] = key->columns;
	header[9] = key->lines;
	write_le16(header + 10U, CACHE_ENTRY_SIZE);
	write_le64(header + 12U, key->book_size);
	write_le32(header + 20U, key->book_modified);
	memcpy(header + 24U, key->id, PAPERMONO_CONTENT_ID_LENGTH);
	write_le32(header + CACHE_HEADER_SIZE - 4U, header_checksum(header));
}

static bool header_matches(const uint8_t *header,
	const struct papermono_reader_cache_key *key)
{
	return memcmp(header, cache_magic, sizeof(cache_magic)) == 0 &&
		header[4] == PAPERMONO_READER_CACHE_FORMAT_VERSION &&
		header[5] == key->kind && header[6] == key->encoding &&
		header[7] == key->density && header[8] == key->columns &&
		header[9] == key->lines &&
		read_le16(header + 10U) == CACHE_ENTRY_SIZE &&
		read_le64(header + 12U) == key->book_size &&
		read_le32(header + 20U) == key->book_modified &&
		memcmp(header + 24U, key->id, PAPERMONO_CONTENT_ID_LENGTH) == 0 &&
		read_le32(header + CACHE_HEADER_SIZE - 4U) ==
			header_checksum(header);
}

static void encode_entry(const struct papermono_reader_cache_entry *entry,
	uint8_t *encoded)
{
	memset(encoded, 0, CACHE_ENTRY_SIZE);
	write_le32(encoded, entry->offset);
	write_le16(encoded + 4U, entry->chapter);
}

static void decode_entry(const uint8_t *encoded,
	struct papermono_reader_cache_entry *entry)
{
	entry->offset = read_le32(encoded);
	entry->chapter = read_le16(encoded + 4U);
}

static bool entries_equal(const struct papermono_reader_cache_entry *left,
	const struct papermono_reader_cache_entry *right)
{
	return left->offset == right->offset && left->chapter == right->chapter;
}

static int read_exact(struct fs_file_t *file, void *buffer, size_t length)
{
	const ssize_t received = fs_read(file, buffer, length);

	return received < 0 ? (int)received :
		received == (ssize_t)length ? 0 : -EBADMSG;
}

static int write_exact(struct fs_file_t *file, const void *buffer, size_t length)
{
	const ssize_t written = fs_write(file, buffer, length);

	return written < 0 ? (int)written :
		written == (ssize_t)length ? 0 : -EIO;
}

static int cache_path(char *path, size_t path_size,
	const struct papermono_reader_cache_key *key)
{
	if (strnlen(key->id, sizeof(key->id)) != PAPERMONO_CONTENT_ID_LENGTH) {
		return -EINVAL;
	}
	const int length = snprintf(path, path_size, "%s/%s.pages",
		PAPERMONO_READER_CACHE_DIRECTORY, key->id);

	return length < 0 || length >= (int)path_size ? -ENAMETOOLONG : 0;
}

bool papermono_reader_cache_matches(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key)
{
	return cache != NULL && cache->ready && key != NULL &&
		cache->key.book_size == key->book_size &&
		cache->key.book_modified == key->book_modified &&
		cache->key.kind == key->kind &&
		cache->key.encoding == key->encoding &&
		cache->key.density == key->density &&
		cache->key.columns == key->columns &&
		cache->key.lines == key->lines &&
		strcmp(cache->key.id, key->id) == 0;
}

void papermono_reader_cache_disable(struct papermono_reader_cache *cache)
{
	if (cache != NULL) {
		cache->ready = false;
		cache->count = 0U;
	}
}

int papermono_reader_cache_reset(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial)
{
	struct papermono_reader_cache_key key_copy;
	uint8_t header[CACHE_HEADER_SIZE];
	uint8_t encoded[CACHE_ENTRY_SIZE];
	char temporary[PAPERMONO_CONTENT_PATH_MAX];
	struct fs_file_t file;
	int ret;

	if (cache == NULL || key == NULL || initial == NULL) {
		return -EINVAL;
	}
	/* Keep reset safe even when the caller passes &cache->key. */
	key_copy = *key;
	memset(cache, 0, sizeof(*cache));
	ret = cache_path(cache->path, sizeof(cache->path), &key_copy);
	if (ret < 0) {
		return ret;
	}
	if (snprintf(temporary, sizeof(temporary), "%s.tmp", cache->path) >=
	    (int)sizeof(temporary)) {
		return -ENAMETOOLONG;
	}
	cache->key = key_copy;
	encode_header(&key_copy, header);
	encode_entry(initial, encoded);
	(void)fs_unlink(temporary);
	fs_file_t_init(&file);
	ret = fs_open(&file, temporary, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		goto fail;
	}
	ret = write_exact(&file, header, sizeof(header));
	ret = ret ?: write_exact(&file, encoded, sizeof(encoded));
	ret = ret ?: fs_sync(&file);
	{
		const int close_ret = fs_close(&file);

		ret = ret ?: close_ret;
	}
	if (ret < 0) {
		goto fail;
	}
	ret = fs_unlink(cache->path);
	if (ret < 0 && ret != -ENOENT) {
		goto fail;
	}
	ret = fs_rename(temporary, cache->path);
	if (ret < 0) {
		goto fail;
	}
	cache->count = 1U;
	cache->ready = true;
	return 0;

fail:
	(void)fs_unlink(temporary);
	papermono_reader_cache_disable(cache);
	return ret;
}

int papermono_reader_cache_open(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial)
{
	uint8_t header[CACHE_HEADER_SIZE];
	struct fs_dirent stat;
	struct fs_file_t file;
	int ret;

	if (cache == NULL || key == NULL || initial == NULL) {
		return -EINVAL;
	}
	memset(cache, 0, sizeof(*cache));
	ret = cache_path(cache->path, sizeof(cache->path), key);
	if (ret < 0) {
		return ret;
	}
	ret = fs_stat(cache->path, &stat);
	if (ret == -ENOENT) {
		return papermono_reader_cache_reset(cache, key, initial);
	}
	if (ret < 0) {
		return ret;
	}
	if (stat.size < CACHE_HEADER_SIZE + CACHE_ENTRY_SIZE ||
	    (stat.size - CACHE_HEADER_SIZE) % CACHE_ENTRY_SIZE != 0U) {
		return papermono_reader_cache_reset(cache, key, initial);
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, cache->path, FS_O_READ);
	if (ret < 0) {
		return ret;
	}
	ret = read_exact(&file, header, sizeof(header));
	{
		const int close_ret = fs_close(&file);

		ret = ret ?: close_ret;
	}
	if (ret < 0 || !header_matches(header, key)) {
		return papermono_reader_cache_reset(cache, key, initial);
	}
	cache->key = *key;
	cache->count = (uint32_t)((stat.size - CACHE_HEADER_SIZE) /
		CACHE_ENTRY_SIZE);
	cache->ready = true;
	return 0;
}

int papermono_reader_cache_read(const struct papermono_reader_cache *cache,
	uint32_t index, struct papermono_reader_cache_entry *entry)
{
	uint8_t encoded[CACHE_ENTRY_SIZE];
	struct fs_file_t file;
	int ret;

	if (cache == NULL || !cache->ready || entry == NULL ||
	    index >= cache->count) {
		return -EINVAL;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, cache->path, FS_O_READ);
	ret = ret ?: fs_seek(&file,
		(off_t)(CACHE_HEADER_SIZE + index * CACHE_ENTRY_SIZE), FS_SEEK_SET);
	ret = ret ?: read_exact(&file, encoded, sizeof(encoded));
	{
		const int close_ret = fs_close(&file);

		ret = ret ?: close_ret;
	}
	if (ret == 0) {
		decode_entry(encoded, entry);
	}
	return ret;
}

int papermono_reader_cache_find(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_entry *entry, uint32_t *index)
{
	uint8_t encoded[CACHE_ENTRY_SIZE];
	struct fs_file_t file;
	int ret;

	if (cache == NULL || !cache->ready || entry == NULL || index == NULL) {
		return -EINVAL;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, cache->path, FS_O_READ);
	ret = ret ?: fs_seek(&file, CACHE_HEADER_SIZE, FS_SEEK_SET);
	for (uint32_t current = 0U; ret == 0 && current < cache->count;
	     ++current) {
		struct papermono_reader_cache_entry candidate;

		ret = read_exact(&file, encoded, sizeof(encoded));
		if (ret < 0) {
			break;
		}
		decode_entry(encoded, &candidate);
		if (entries_equal(&candidate, entry)) {
			*index = current;
			(void)fs_close(&file);
			return 0;
		}
	}
	(void)fs_close(&file);
	return ret < 0 ? ret : -ENOENT;
}

int papermono_reader_cache_record(struct papermono_reader_cache *cache,
	uint32_t index, const struct papermono_reader_cache_entry *entry)
{
	struct papermono_reader_cache_entry existing;
	uint8_t encoded[CACHE_ENTRY_SIZE];
	struct fs_file_t file;
	bool truncate = false;
	int ret;

	if (cache == NULL || !cache->ready || entry == NULL ||
	    index > cache->count) {
		return -EINVAL;
	}
	if (index < cache->count) {
		ret = papermono_reader_cache_read(cache, index, &existing);
		if (ret < 0) {
			return ret;
		}
		if (entries_equal(&existing, entry)) {
			return 0;
		}
		truncate = true;
	}
	encode_entry(entry, encoded);
	fs_file_t_init(&file);
	ret = fs_open(&file, cache->path, FS_O_WRITE);
	if (ret < 0) {
		return ret;
	}
	if (truncate) {
		ret = fs_truncate(&file,
			(off_t)(CACHE_HEADER_SIZE + index * CACHE_ENTRY_SIZE));
	}
	ret = ret ?: fs_seek(&file, 0, FS_SEEK_END);
	ret = ret ?: write_exact(&file, encoded, sizeof(encoded));
	ret = ret ?: fs_sync(&file);
	{
		const int close_ret = fs_close(&file);

		ret = ret ?: close_ret;
	}
	if (ret == 0) {
		cache->count = index + 1U;
	}
	return ret;
}
