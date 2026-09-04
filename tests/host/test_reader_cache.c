/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>

#include <papermono/reader_cache.h>

#define FAKE_FILE_COUNT 4U
#define FAKE_FILE_CAPACITY 4096U

struct fake_file {
	char path[PAPERMONO_CONTENT_PATH_MAX];
	uint8_t data[FAKE_FILE_CAPACITY];
	size_t size;
	bool exists;
};

static struct fake_file fake_files[FAKE_FILE_COUNT];

static int find_file(const char *path)
{
	for (size_t index = 0U; index < FAKE_FILE_COUNT; ++index) {
		if (fake_files[index].exists &&
		    strcmp(fake_files[index].path, path) == 0) {
			return (int)index;
		}
	}
	return -1;
}

static int create_file(const char *path)
{
	for (size_t index = 0U; index < FAKE_FILE_COUNT; ++index) {
		if (!fake_files[index].exists) {
			memset(&fake_files[index], 0, sizeof(fake_files[index]));
			fake_files[index].exists = true;
			(void)snprintf(fake_files[index].path,
				sizeof(fake_files[index].path), "%s", path);
			return (int)index;
		}
	}
	return -ENOSPC;
}

int fs_open(struct fs_file_t *file, const char *path, int flags)
{
	int slot = find_file(path);

	if (slot < 0 && (flags & FS_O_CREATE) != 0) {
		slot = create_file(path);
	}
	if (slot < 0) {
		return -ENOENT;
	}
	if ((flags & FS_O_TRUNC) != 0) {
		fake_files[slot].size = 0U;
	}
	file->slot = slot;
	file->flags = flags;
	file->offset = (flags & FS_O_APPEND) != 0 ? fake_files[slot].size : 0U;
	return 0;
}

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	if (file->slot < 0 || (file->flags & FS_O_READ) == 0) {
		return -EBADF;
	}
	const size_t remaining = file->offset < fake_files[file->slot].size ?
		fake_files[file->slot].size - file->offset : 0U;
	const size_t copied = remaining < length ? remaining : length;

	memcpy(buffer, fake_files[file->slot].data + file->offset, copied);
	file->offset += copied;
	return (ssize_t)copied;
}

ssize_t fs_write(struct fs_file_t *file, const void *buffer, size_t length)
{
	if (file->slot < 0 || (file->flags & FS_O_WRITE) == 0) {
		return -EBADF;
	}
	if (file->offset + length > FAKE_FILE_CAPACITY) {
		return -ENOSPC;
	}
	memcpy(fake_files[file->slot].data + file->offset, buffer, length);
	file->offset += length;
	if (file->offset > fake_files[file->slot].size) {
		fake_files[file->slot].size = file->offset;
	}
	return (ssize_t)length;
}

int fs_seek(struct fs_file_t *file, off_t offset, int whence)
{
	if (file->slot < 0) {
		return -EBADF;
	}
	off_t base = whence == FS_SEEK_SET ? 0 :
		whence == FS_SEEK_CUR ? (off_t)file->offset :
		whence == FS_SEEK_END ? (off_t)fake_files[file->slot].size : -1;

	if (base < 0 || offset < -base ||
	    (size_t)(base + offset) > fake_files[file->slot].size) {
		return -EINVAL;
	}
	file->offset = (size_t)(base + offset);
	return 0;
}

int fs_truncate(struct fs_file_t *file, off_t length)
{
	if (file->slot < 0 || length < 0 ||
	    (size_t)length > fake_files[file->slot].size) {
		return -EINVAL;
	}
	fake_files[file->slot].size = (size_t)length;
	if (file->offset > (size_t)length) {
		file->offset = (size_t)length;
	}
	return 0;
}

int fs_sync(struct fs_file_t *file)
{
	return file->slot < 0 ? -EBADF : 0;
}

int fs_close(struct fs_file_t *file)
{
	file->slot = -1;
	return 0;
}

int fs_stat(const char *path, struct fs_dirent *entry)
{
	const int slot = find_file(path);

	if (slot < 0) {
		return -ENOENT;
	}
	entry->size = fake_files[slot].size;
	return 0;
}

int fs_unlink(const char *path)
{
	const int slot = find_file(path);

	if (slot < 0) {
		return -ENOENT;
	}
	fake_files[slot].exists = false;
	return 0;
}

int fs_rename(const char *from, const char *to)
{
	const int slot = find_file(from);

	if (slot < 0 || find_file(to) >= 0) {
		return slot < 0 ? -ENOENT : -EEXIST;
	}
	(void)snprintf(fake_files[slot].path, sizeof(fake_files[slot].path),
		"%s", to);
	return 0;
}

static struct papermono_reader_cache_key make_key(void)
{
	struct papermono_reader_cache_key key = {
		.book_size = 123456U,
		.book_modified = 0x12345678U,
		.kind = PAPERMONO_CONTENT_TXT,
		.encoding = 1U,
		.density = 1U,
		.columns = 26U,
		.lines = 30U,
	};

	(void)snprintf(key.id, sizeof(key.id), "0123456789abcdef");
	return key;
}

static void test_create_reopen_and_branch(void)
{
	struct papermono_reader_cache_key key = make_key();
	struct papermono_reader_cache cache;
	struct papermono_reader_cache reopened;
	struct papermono_reader_cache_entry entry = { 0 };
	struct papermono_reader_cache_entry read;
	uint32_t index;

	memset(fake_files, 0, sizeof(fake_files));
	assert(papermono_reader_cache_open(&cache, &key, &entry) == 0);
	assert(cache.ready && cache.count == 1U);
	entry.offset = 700U;
	assert(papermono_reader_cache_record(&cache, 1U, &entry) == 0);
	entry.offset = 1400U;
	assert(papermono_reader_cache_record(&cache, 2U, &entry) == 0);
	assert(cache.count == 3U);

	entry.offset = 700U;
	assert(papermono_reader_cache_open(&reopened, &key, &entry) == 0);
	assert(reopened.count == 3U);
	assert(papermono_reader_cache_find(&reopened, &entry, &index) == 0);
	assert(index == 1U);

	entry.offset = 900U;
	assert(papermono_reader_cache_record(&reopened, 1U, &entry) == 0);
	assert(reopened.count == 2U);
	assert(papermono_reader_cache_read(&reopened, 1U, &read) == 0);
	assert(read.offset == 900U);
}

static void test_corruption_and_layout_invalidate(void)
{
	struct papermono_reader_cache_key key = make_key();
	struct papermono_reader_cache cache;
	struct papermono_reader_cache_entry initial = { .offset = 77U };
	int slot;

	memset(fake_files, 0, sizeof(fake_files));
	assert(papermono_reader_cache_open(&cache, &key, &initial) == 0);
	slot = find_file(cache.path);
	assert(slot >= 0);
	fake_files[slot].data[0] ^= 0xFFU;
	initial.offset = 99U;
	assert(papermono_reader_cache_open(&cache, &key, &initial) == 0);
	assert(cache.count == 1U);
	struct papermono_reader_cache_entry read;
	assert(papermono_reader_cache_read(&cache, 0U, &read) == 0);
	assert(read.offset == 99U);

	key.density = 2U;
	initial.offset = 123U;
	assert(papermono_reader_cache_open(&cache, &key, &initial) == 0);
	assert(cache.count == 1U && cache.key.density == 2U);
	assert(papermono_reader_cache_read(&cache, 0U, &read) == 0);
	assert(read.offset == 123U);

	slot = find_file(cache.path);
	assert(slot >= 0 && fake_files[slot].size > 0U);
	--fake_files[slot].size;
	initial.offset = 456U;
	assert(papermono_reader_cache_open(&cache, &key, &initial) == 0);
	assert(cache.count == 1U);
	assert(papermono_reader_cache_read(&cache, 0U, &read) == 0);
	assert(read.offset == 456U);
}

int main(void)
{
	test_create_reopen_and_branch();
	test_corruption_and_layout_invalidate();
	puts("reader cache tests: PASS");
	return 0;
}
