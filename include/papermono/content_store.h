/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_CONTENT_STORE_H_
#define PAPERMONO_CONTENT_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_SD_MOUNT_POINT "/SD:"
#define PAPERMONO_CONTENT_ROOT PAPERMONO_SD_MOUNT_POINT "/papermono"
#define PAPERMONO_BOOKS_DIRECTORY PAPERMONO_CONTENT_ROOT "/books"
#define PAPERMONO_CALENDAR_DIRECTORY PAPERMONO_CONTENT_ROOT "/calendar"
#define PAPERMONO_TASKS_DIRECTORY PAPERMONO_CONTENT_ROOT "/tasks"
#define PAPERMONO_NOTES_DIRECTORY PAPERMONO_CONTENT_ROOT "/notes"
#define PAPERMONO_WEREAD_DIRECTORY PAPERMONO_CONTENT_ROOT "/weread"
#define PAPERMONO_WEREAD_CATALOG_NAME "weread-v1.json"
#define PAPERMONO_WEREAD_CATALOG_PATH PAPERMONO_WEREAD_DIRECTORY "/" \
	PAPERMONO_WEREAD_CATALOG_NAME
#define PAPERMONO_CACHE_DIRECTORY PAPERMONO_CONTENT_ROOT "/.cache"
#define PAPERMONO_READER_CACHE_DIRECTORY PAPERMONO_CACHE_DIRECTORY "/reader"
#define PAPERMONO_CONTENT_PATH_MAX 384U
#define PAPERMONO_CONTENT_ID_LENGTH 16U
#define PAPERMONO_BOOK_FILE_LIMIT (64ULL * 1024ULL * 1024ULL)
#define PAPERMONO_METADATA_FILE_LIMIT (256ULL * 1024ULL)
#define PAPERMONO_WEREAD_FILE_LIMIT (64ULL * 1024ULL)

enum papermono_media_state {
	PAPERMONO_MEDIA_UNINITIALIZED,
	PAPERMONO_MEDIA_READY,
	PAPERMONO_MEDIA_EMPTY,
	PAPERMONO_MEDIA_REMOVED,
	PAPERMONO_MEDIA_UNFORMATTED,
	PAPERMONO_MEDIA_ERROR,
};

enum papermono_content_kind {
	PAPERMONO_CONTENT_TXT,
	PAPERMONO_CONTENT_EPUB,
	PAPERMONO_CONTENT_ICS,
	PAPERMONO_CONTENT_TASKS,
	PAPERMONO_CONTENT_NOTE,
	PAPERMONO_CONTENT_WEREAD,
	PAPERMONO_CONTENT_UNKNOWN,
};

struct papermono_content_entry {
	char id[PAPERMONO_CONTENT_ID_LENGTH + 1U];
	char path[PAPERMONO_CONTENT_PATH_MAX];
	char title[MAX_FILE_NAME + 1U];
	enum papermono_content_kind kind;
	uint64_t size;
	uint32_t modified;
};

struct papermono_content_transaction {
	struct fs_file_t file;
	char final_path[PAPERMONO_CONTENT_PATH_MAX];
	char upload_path[PAPERMONO_CONTENT_PATH_MAX];
	char backup_path[PAPERMONO_CONTENT_PATH_MAX];
	uint64_t expected_size;
	uint64_t written;
	uint32_t expected_crc32;
	uint32_t crc32;
	enum papermono_content_kind kind;
	bool verify_crc32;
	bool active;
	bool file_open;
};

typedef int (*papermono_content_scan_cb)(
	const struct papermono_content_entry *entry, void *context);

int papermono_content_store_init(void);
int papermono_content_store_mount(void);
int papermono_content_store_unmount(void);
int papermono_content_store_poll(void);
enum papermono_media_state papermono_content_store_media_state(void);
const char *papermono_content_store_state_text(enum papermono_media_state state);
int papermono_content_store_scan_books(papermono_content_scan_cb callback,
	void *context, size_t *count);
int papermono_content_store_scan_all(papermono_content_scan_cb callback,
	void *context, size_t *count);
int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file);
int papermono_content_store_import_begin(
	struct papermono_content_transaction *transaction,
	enum papermono_content_kind kind, const char *name, uint64_t size,
	bool verify_crc32, uint32_t expected_crc32);
int papermono_content_store_import_write(
	struct papermono_content_transaction *transaction,
	const uint8_t *data, size_t length);
int papermono_content_store_import_commit(
	struct papermono_content_transaction *transaction);
int papermono_content_store_import_abort(
	struct papermono_content_transaction *transaction);
int papermono_content_store_delete(const struct papermono_content_entry *entry);
bool papermono_content_store_safe_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_CONTENT_STORE_H_ */
