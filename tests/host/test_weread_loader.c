/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <papermono/content_store.h>
#include <papermono/weread.h>

static const char *catalog_data;
static uint64_t catalog_size;
static int scan_error;
static bool short_read;

int papermono_content_store_scan_all(papermono_content_scan_cb callback,
	void *context, size_t *count)
{
	if (scan_error < 0) {
		return scan_error;
	}
	if (catalog_data == NULL) {
		if (count != NULL) {
			*count = 0U;
		}
		return 0;
	}
	const struct papermono_content_entry entry = {
		.path = PAPERMONO_WEREAD_CATALOG_PATH,
		.kind = PAPERMONO_CONTENT_WEREAD,
		.size = catalog_size,
	};
	const int ret = callback(&entry, context);

	if (count != NULL) {
		*count = ret == 0 ? 1U : 0U;
	}
	return ret;
}

int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file)
{
	assert(entry != NULL && file != NULL);
	fs_file_t_init(file);
	return 0;
}

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	(void)file;
	if (short_read && length > 0U) {
		--length;
	}
	memcpy(buffer, catalog_data, length);
	return (ssize_t)length;
}

int fs_close(struct fs_file_t *file)
{
	(void)file;
	return 0;
}

static void set_catalog(const char *data)
{
	catalog_data = data;
	catalog_size = data == NULL ? 0U : strlen(data);
	short_read = false;
}

int main(void)
{
	static const char valid[] =
		"{\"version\":1,\"source\":\"host-test\","
		"\"synced_at\":\"2026-08-31T18:30:00Z\",\"total\":1,"
		"\"books\":[{\"id\":\"book-1\",\"title\":\"示例\"}]}";
	static const char invalid_utf8[] =
		"{\"version\":1,\"source\":\"\xc0\xaf\",\"synced_at\":"
		"\"2026-08-31T18:30:00Z\",\"total\":0,\"books\":[]}";
	struct papermono_weread_status status;
	struct papermono_weread_book book;

	papermono_weread_get_status(&status);
	assert(status.last_error == -ENOENT && status.count == 0U);

	scan_error = -ENODEV;
	assert(papermono_weread_reload() == -ENODEV);
	papermono_weread_get_status(&status);
	assert(status.last_error == -ENODEV && status.count == 0U);

	scan_error = 0;
	set_catalog(NULL);
	assert(papermono_weread_reload() == -ENOENT);

	set_catalog(valid);
	assert(papermono_weread_reload() == 0);
	papermono_weread_get_status(&status);
	assert(status.last_error == 0 && status.count == 1U && status.total == 1U);
	assert(strcmp(status.source, "host-test") == 0);
	assert(papermono_weread_get(0U, &book) == 0);
	assert(strcmp(book.id, "book-1") == 0);
	assert(papermono_weread_get(1U, &book) == -ENOENT);

	set_catalog(invalid_utf8);
	assert(papermono_weread_reload() == -EILSEQ);
	papermono_weread_get_status(&status);
	assert(status.last_error == -EILSEQ && status.count == 0U);

	set_catalog(valid);
	catalog_size = PAPERMONO_WEREAD_FILE_LIMIT + 1U;
	assert(papermono_weread_reload() == -EFBIG);

	set_catalog(valid);
	short_read = true;
	assert(papermono_weread_reload() == -EIO);
	papermono_weread_get_status(&status);
	assert(status.last_error == -EIO && status.count == 0U);

	set_catalog(valid);
	assert(papermono_weread_reload() == 0);
	puts("WeRead catalog reload state tests: PASS");
	return 0;
}
