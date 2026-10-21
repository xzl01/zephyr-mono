/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ff.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/storage/disk_access.h>

#include <papermono/content_store.h>

static bool card_inserted = true;
static bool mounted;
static bool validation_unmounts;
static bool sync_fails;
static bool readdir_fails;
static unsigned int open_files;
static unsigned int upload_unlinks;
static unsigned int unlink_calls;

/* Minimal existence model for the commit happy path. Empty until a rename
 * produces the final content path, so every older assertion that relies on
 * "nothing exists" keeps its original behaviour.
 */
static char committed_path[256];
static size_t committed_size;
static size_t stub_last_write;

bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	return true;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
	(void)spec;
	(void)flags;
	return 0;
}

int gpio_pin_get_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	return card_inserted ? 1 : 0;
}

int disk_access_ioctl(const char *name, int command, void *buffer)
{
	(void)name;
	(void)command;
	(void)buffer;
	return 0;
}

int disk_access_status(const char *name)
{
	(void)name;
	return card_inserted ? 0 : DISK_STATUS_NOMEDIA;
}

int fs_mount(struct fs_mount_t *mount)
{
	(void)mount;
	mounted = true;
	return 0;
}

int fs_unmount(struct fs_mount_t *mount)
{
	(void)mount;
	mounted = false;
	return 0;
}

/* Only reached through the explicitly-confirmed TF format path; the store tests
 * never format, so record the call and succeed.
 */
static unsigned int fs_mkfs_calls;

int fs_mkfs(int fs_type, uintptr_t dev_id, void *cfg, int flags)
{
	(void)dev_id;
	(void)cfg;
	(void)flags;
	if (fs_type != FS_FATFS) {
		return -EINVAL;
	}
	++fs_mkfs_calls;
	return 0;
}

int fs_mkdir(const char *path)
{
	(void)path;
	return 0;
}

int fs_statvfs(const char *path, struct fs_statvfs *stat)
{
	(void)path;
	stat->f_bsize = 4096U;
	stat->f_bfree = 4096U;
	return 0;
}

int fs_opendir(struct fs_dir_t *directory, const char *path)
{
	(void)directory;
	(void)path;
	return 0;
}

int fs_readdir(struct fs_dir_t *directory, struct fs_dirent *entry)
{
	(void)directory;
	memset(entry, 0, sizeof(*entry));
	return 0;
}

int fs_closedir(struct fs_dir_t *directory)
{
	(void)directory;
	return 0;
}

int fs_open(struct fs_file_t *file, const char *path, int flags)
{
	(void)path;
	file->flags = flags;
	file->slot = 1;
	++open_files;
	return 0;
}

ssize_t fs_write(struct fs_file_t *file, const void *buffer, size_t length)
{
	(void)file;
	(void)buffer;
	stub_last_write = length;
	return (ssize_t)length;
}

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	(void)file;
	(void)buffer;
	(void)length;
	return 0;
}

int fs_seek(struct fs_file_t *file, off_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return 0;
}

int fs_truncate(struct fs_file_t *file, off_t length)
{
	(void)file;
	(void)length;
	return 0;
}

int fs_sync(struct fs_file_t *file)
{
	(void)file;
	return sync_fails ? -EIO : 0;
}

int fs_close(struct fs_file_t *file)
{
	(void)file;
	assert(open_files > 0U);
	--open_files;
	return 0;
}

int fs_stat(const char *path, struct fs_dirent *entry)
{
	if (committed_path[0] != '\0' && strcmp(path, committed_path) == 0) {
		if (entry != NULL) {
			memset(entry, 0, sizeof(*entry));
			entry->size = committed_size;
			entry->type = FS_DIR_ENTRY_FILE;
		}
		return 0;
	}
	(void)path;
	(void)entry;
	return -ENOENT;
}

int fs_unlink(const char *path)
{
	++unlink_calls;
	if (strstr(path, ".upload") != NULL) {
		++upload_unlinks;
	}
	return 0;
}

int fs_rename(const char *from, const char *to)
{
	(void)from;
	/* Only the commit's upload -> final rename publishes a visible file. */
	if (strstr(to, ".upload") == NULL && strstr(to, ".backup") == NULL) {
		(void)snprintf(committed_path, sizeof(committed_path), "%s", to);
		committed_size = stub_last_write;
	}
	return 0;
}

/* Directory entries handed to the scanner. When empty the directory reads as
 * end-of-list, which is what the older tests expect.
 */
struct fake_dir_entry {
	const char *name;
	uint32_t size;
	uint32_t modified;
	uint8_t attrib;
};

static const struct fake_dir_entry *fake_dir_entries;
static size_t fake_dir_count;
static size_t fake_dir_pos;

FRESULT f_opendir(DIR *directory, const char *path)
{
	(void)directory;
	(void)path;
	fake_dir_pos = 0U;
	return FR_OK;
}

FRESULT f_readdir(DIR *directory, FILINFO *info)
{
	(void)directory;
	if (readdir_fails) {
		readdir_fails = false;
		return 1;
	}
	memset(info, 0, sizeof(*info));
	if (fake_dir_entries == NULL || fake_dir_pos >= fake_dir_count) {
		return FR_OK;
	}
	const struct fake_dir_entry *entry = &fake_dir_entries[fake_dir_pos++];

	snprintf(info->fname, sizeof(info->fname), "%s", entry->name);
	info->fsize = entry->size;
	info->fdate = (uint16_t)(entry->modified >> 16);
	info->ftime = (uint16_t)entry->modified;
	info->fattrib = entry->attrib;
	return FR_OK;
}

FRESULT f_closedir(DIR *directory)
{
	(void)directory;
	return FR_OK;
}

bool papermono_text_utf8_valid(const uint8_t *data, size_t length,
	bool allow_bom)
{
	(void)data;
	(void)length;
	(void)allow_bom;
	return true;
}

int papermono_content_validate(enum papermono_content_kind kind,
	const char *path, uint64_t size)
{
	(void)kind;
	(void)path;
	(void)size;
	if (validation_unmounts) {
		validation_unmounts = false;
		assert(papermono_content_store_unmount() == 0);
	}
	return 0;
}

static int count_entry(const struct papermono_content_entry *entry,
	void *context)
{
	unsigned int *count = context;

	(void)entry;
	++*count;
	return 0;
}

static struct papermono_content_entry scanned[8];
static size_t scanned_count;

static int collect_entry(const struct papermono_content_entry *entry,
	void *context)
{
	(void)context;
	if (scanned_count < sizeof(scanned) / sizeof(scanned[0])) {
		scanned[scanned_count++] = *entry;
	}
	return 0;
}

/* The scan must report each supported file with the timestamp taken from the
 * directory entry itself, and must skip subdirectories and unknown types.
 */
static void test_scan_uses_directory_entries(void)
{
	static const struct fake_dir_entry entries[] = {
		{ "alpha.txt", 100U, 0x12340001U, 0U },
		{ "beta.epub", 200U, 0x56780002U, 0U },
		{ "subdir", 0U, 0U, AM_DIR },
		{ "ignored.pdf", 300U, 0x00000000U, 0U },
	};
	size_t count = 0U;

	fake_dir_entries = entries;
	fake_dir_count = sizeof(entries) / sizeof(entries[0]);
	scanned_count = 0U;
	assert(papermono_content_store_scan_books(collect_entry, NULL,
		&count) == 0);
	fake_dir_entries = NULL;
	fake_dir_count = 0U;

	assert(count == 2U);
	assert(scanned_count == 2U);
	assert(scanned[0].size == 100U);
	assert(scanned[1].size == 200U);
	/* Timestamps come from fdate/ftime, not a per-file f_stat lookup. */
	assert(scanned[0].modified == 0x12340001U);
	assert(scanned[1].modified == 0x56780002U);
	/* Distinct content, so distinct stable ids. */
	assert(strcmp(scanned[0].id, scanned[1].id) != 0);
	assert(strstr(scanned[0].path, "alpha.txt") != NULL);
}

int main(void)
{
	static const uint8_t data[] = { 1U, 2U, 3U, 4U };
	struct papermono_content_transaction first;
	struct papermono_content_transaction second;
	struct papermono_content_entry entry = {
		.kind = PAPERMONO_CONTENT_TXT,
	};
	struct fs_file_t read_file;
	unsigned int unlink_calls_before;
	char long_name[MAX_FILE_NAME + 2U];
	size_t scanned = 99U;
	unsigned int callbacks = 0U;

	assert(papermono_content_store_init() == 0);
	assert(mounted);
	test_scan_uses_directory_entries();
	readdir_fails = true;
	assert(papermono_content_store_scan_books(count_entry, &callbacks,
		&scanned) == -EIO);
	assert(scanned == 0U && callbacks == 0U);
	assert(papermono_content_store_import_begin(&first, PAPERMONO_CONTENT_TXT,
		"book.txt", sizeof(data), false, 0U) == 0);
	assert(first.active && first.file_open && open_files == 1U);
	assert(papermono_content_store_import_begin(&second, PAPERMONO_CONTENT_TXT,
		"second.txt", sizeof(data), false, 0U) == -EBUSY);
	assert(strstr(first.upload_path, "/.upload/book.txt") != NULL);
	assert(strstr(first.backup_path, "/.backup/book.txt") != NULL);
	assert(papermono_content_store_import_write(&first, data, 2U) == 0);

	card_inserted = false;
	assert(papermono_content_store_poll() == -ENODEV);
	assert(!first.active && !first.file_open && open_files == 0U);
	assert(papermono_content_store_import_write(&first, data, 1U) == -ENODEV);
	assert(papermono_content_store_import_commit(&first) == -ENODEV);
	assert(papermono_content_store_import_abort(&first) == -ENODEV);
	assert(upload_unlinks > 0U);

	card_inserted = true;
	assert(papermono_content_store_mount() == 0);
	assert(papermono_content_store_import_begin(&first, PAPERMONO_CONTENT_TXT,
		"book.txt", sizeof(data), false, 0U) == 0);
	assert(papermono_content_store_import_write(&first, data,
		sizeof(data) + 1U) == -EINVAL);
	assert(first.written == 0U);
	assert(papermono_content_store_import_write(&first, data, sizeof(data)) == 0);
	validation_unmounts = true;
	assert(papermono_content_store_import_commit(&first) == -ENODEV);
	assert(!first.active && !first.file_open && open_files == 0U);

	assert(papermono_content_store_import_begin(&first, PAPERMONO_CONTENT_NOTE,
		"large.md", PAPERMONO_METADATA_FILE_LIMIT + 1U, false, 0U) == -EINVAL);
	assert(papermono_content_store_mount() == 0);
	assert(papermono_content_store_import_begin(&first, PAPERMONO_CONTENT_TXT,
		"sync-error.txt", sizeof(data), false, 0U) == 0);
	assert(papermono_content_store_import_write(&first, data, sizeof(data)) == 0);
	sync_fails = true;
	assert(papermono_content_store_import_commit(&first) == -EIO);
	assert(!first.active && !first.file_open && open_files == 0U);

	/* Successful commit of a brand-new file: the path the product uses for
	 * every first upload. Before the unlink guard fix this logged
	 * "failed to unlink path (-2)" because the stale-backup cleanup ran
	 * unconditionally.
	 */
	assert(papermono_content_store_mount() == 0);
	sync_fails = false;
	committed_path[0] = '\0';
	unlink_calls_before = unlink_calls;
	assert(papermono_content_store_import_begin(&first, PAPERMONO_CONTENT_TXT,
		"commit-ok.txt", sizeof(data), false, 0U) == 0);
	assert(papermono_content_store_import_write(&first, data,
		sizeof(data)) == 0);
	assert(papermono_content_store_import_commit(&first) == 0);
	assert(!first.active && !first.file_open && open_files == 0U);
	assert(strstr(committed_path, "commit-ok.txt") != NULL);
	assert(committed_size == sizeof(data));
	/* No stale backup existed, so no unlink should have been attempted. */
	assert(unlink_calls == unlink_calls_before);

	memset(long_name, 'a', MAX_FILE_NAME - 4U);
	memcpy(long_name + MAX_FILE_NAME - 4U, ".txt", 5U);
	assert(strlen(long_name) == MAX_FILE_NAME);
	assert(papermono_content_store_import_begin(&first,
		PAPERMONO_CONTENT_TXT, long_name, 0U, false, 0U) == 0);
	assert(strlen(strrchr(first.final_path, '/') + 1U) == MAX_FILE_NAME);
	assert(papermono_content_store_import_abort(&first) == 0);
	memmove(long_name + 1U, long_name, MAX_FILE_NAME + 1U);
	long_name[0] = 'b';
	assert(strlen(long_name) == MAX_FILE_NAME + 1U);
	assert(papermono_content_store_import_begin(&first,
		PAPERMONO_CONTENT_TXT, long_name, 0U, false, 0U) == -EINVAL);
	assert(papermono_content_store_import_begin(&first,
		PAPERMONO_CONTENT_WEREAD, "shelf.json", sizeof(data), false, 0U) ==
		-EINVAL);
	assert(papermono_content_store_import_begin(&first,
		PAPERMONO_CONTENT_WEREAD, PAPERMONO_WEREAD_CATALOG_NAME,
		PAPERMONO_WEREAD_FILE_LIMIT + 1U, false, 0U) == -EINVAL);
	assert(papermono_content_store_import_begin(&first,
		PAPERMONO_CONTENT_WEREAD, PAPERMONO_WEREAD_CATALOG_NAME,
		sizeof(data), false, 0U) == 0);
	assert(strstr(first.final_path, "/weread/weread-v1.json") != NULL);
	assert(papermono_content_store_import_abort(&first) == 0);

	(void)snprintf(entry.path, sizeof(entry.path), "%s/safe.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == 0);
	assert(fs_close(&read_file) == 0);
	unlink_calls_before = unlink_calls;
	(void)snprintf(entry.path, sizeof(entry.path), "%s/../secret.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	assert(papermono_content_store_delete(&entry) == -EPERM);
	assert(unlink_calls == unlink_calls_before);
	(void)snprintf(entry.path, sizeof(entry.path), "%s/..\\secret.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	assert(papermono_content_store_delete(&entry) == -EPERM);
	assert(unlink_calls == unlink_calls_before);
	(void)snprintf(entry.path, sizeof(entry.path), "%s/nested/secret.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	assert(papermono_content_store_delete(&entry) == -EPERM);
	assert(unlink_calls == unlink_calls_before);
	(void)snprintf(entry.path, sizeof(entry.path), "%s/nested\\secret.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	assert(papermono_content_store_delete(&entry) == -EPERM);
	assert(unlink_calls == unlink_calls_before);
	(void)snprintf(entry.path, sizeof(entry.path), "%s/wrong.epub",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	assert(papermono_content_store_delete(&entry) == -EPERM);
	assert(unlink_calls == unlink_calls_before);

	/* Import validation reopens the EPUB from the staging directory, so the
	 * "<dir>/.upload/<name>" shape has to be openable. The leaf name is still
	 * checked exactly as a published file is, so the staging prefix cannot be
	 * used to reach anything else.
	 */
	entry.kind = PAPERMONO_CONTENT_EPUB;
	(void)snprintf(entry.path, sizeof(entry.path), "%s/.upload/book.epub",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == 0);
	assert(fs_close(&read_file) == 0);
	/* Traversal through the staging directory is still refused. */
	(void)snprintf(entry.path, sizeof(entry.path),
		"%s/.upload/../book.epub", PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	(void)snprintf(entry.path, sizeof(entry.path),
		"%s/.upload/../../secret.epub", PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	/* A staging path must not smuggle a second directory in. */
	(void)snprintf(entry.path, sizeof(entry.path),
		"%s/.upload/nested/book.epub", PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	/* The kind still has to match the name. */
	(void)snprintf(entry.path, sizeof(entry.path), "%s/.upload/book.txt",
		PAPERMONO_BOOKS_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);
	/* Staging is only accepted under the kind's own directory. */
	(void)snprintf(entry.path, sizeof(entry.path), "%s/.upload/book.epub",
		PAPERMONO_NOTES_DIRECTORY);
	assert(papermono_content_store_open(&entry, &read_file) == -EINVAL);

	/* A card with no mountable FAT is provisioned by the explicit format path;
	 * that path is the only caller of fs_mkfs().
	 */
	assert(papermono_content_store_unmount() == 0);
	assert(papermono_content_store_format() == 0);
	assert(fs_mkfs_calls == 1U);
	assert(mounted);
	puts("content store hot-remove transactions: PASS");
	return 0;
}
