/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <papermono/content_store.h>
#include <papermono/fs_read.h>
#include <papermono/personal_data.h>
#include <papermono/text_codec.h>
#include <papermono/weread.h>

LOG_MODULE_REGISTER(papermono_weread, LOG_LEVEL_INF);

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_WEREAD_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_WEREAD_BSS
#endif

static struct papermono_weread_catalog catalog PAPERMONO_WEREAD_BSS;
static struct papermono_weread_catalog loading_catalog PAPERMONO_WEREAD_BSS;
static char catalog_buffer[PAPERMONO_WEREAD_FILE_LIMIT + 1U]
	PAPERMONO_WEREAD_BSS;
static int catalog_error = -ENOENT;
K_MUTEX_DEFINE(weread_lock);
K_MUTEX_DEFINE(weread_reload_lock);

struct find_catalog_context {
	struct papermono_content_entry entry;
	bool found;
};

static int find_catalog(const struct papermono_content_entry *entry,
	void *context)
{
	struct find_catalog_context *result = context;

	if (entry->kind == PAPERMONO_CONTENT_WEREAD) {
		result->entry = *entry;
		result->found = true;
	}
	return 0;
}

static void clear_catalog(int error)
{
	k_mutex_lock(&weread_lock, K_FOREVER);
	memset(&catalog, 0, sizeof(catalog));
	catalog_error = error;
	k_mutex_unlock(&weread_lock);
}

static int reload_catalog(void)
{
	struct find_catalog_context found = { 0 };
	struct fs_file_t file;
	ssize_t received;
	int ret = papermono_content_store_scan_all(find_catalog, &found, NULL);

	if (ret < 0) {
		clear_catalog(ret);
		return ret;
	}
	if (!found.found) {
		clear_catalog(-ENOENT);
		return -ENOENT;
	}
	if (found.entry.size > PAPERMONO_WEREAD_FILE_LIMIT) {
		clear_catalog(-EFBIG);
		return -EFBIG;
	}
	fs_file_t_init(&file);
	ret = papermono_content_store_open(&found.entry, &file);
	if (ret < 0) {
		clear_catalog(ret);
		return ret;
	}
	received = papermono_fs_read_chunked(&file, catalog_buffer, (size_t)found.entry.size);
	(void)fs_close(&file);
	if (received < 0 || (uint64_t)received != found.entry.size) {
		ret = received < 0 ? (int)received : -EIO;
		clear_catalog(ret);
		return ret;
	}
	catalog_buffer[received] = '\0';
	if (!papermono_text_utf8_valid((const uint8_t *)catalog_buffer,
		(size_t)received, false)) {
		clear_catalog(-EILSEQ);
		return -EILSEQ;
	}
	ret = papermono_parse_weread_json(catalog_buffer, (size_t)received,
		&loading_catalog);
	if (ret < 0) {
		clear_catalog(ret);
		return ret;
	}
	k_mutex_lock(&weread_lock, K_FOREVER);
	catalog = loading_catalog;
	catalog_error = 0;
	k_mutex_unlock(&weread_lock);
	LOG_INF("WeRead catalog loaded: visible=%zu total=%zu synced=%s",
		catalog.count, catalog.total, catalog.synced_at);
	return 0;
}

int papermono_weread_reload(void)
{
	int ret;

	k_mutex_lock(&weread_reload_lock, K_FOREVER);
	ret = reload_catalog();
	k_mutex_unlock(&weread_reload_lock);
	return ret;
}

void papermono_weread_get_status(struct papermono_weread_status *status)
{
	if (status == NULL) {
		return;
	}
	k_mutex_lock(&weread_lock, K_FOREVER);
	memset(status, 0, sizeof(*status));
	memcpy(status->source, catalog.source, sizeof(status->source));
	memcpy(status->synced_at, catalog.synced_at, sizeof(status->synced_at));
	status->count = catalog.count;
	status->total = catalog.total;
	status->truncated = catalog.truncated;
	status->last_error = catalog_error;
	k_mutex_unlock(&weread_lock);
}

int papermono_weread_get(size_t index, struct papermono_weread_book *book)
{
	int ret = 0;

	if (book == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&weread_lock, K_FOREVER);
	if (index >= catalog.count) {
		ret = -ENOENT;
	} else {
		*book = catalog.books[index];
	}
	k_mutex_unlock(&weread_lock);
	return ret;
}
