/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host implementations of the Zephyr APIs that PAPER MONO's portable modules
 * use.  Clean-room, preview-only: nothing here is derived from Zephyr.
 *
 * The filesystem shim roots "/SD:" at $PAPERMONO_PREVIEW_FS (default
 * /tmp/papermono-preview/build/fs) so papermono_content_store.c can walk a real
 * directory tree with real content files.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>

/* ------------------------------------------------------------------ time */

int64_t k_uptime_get(void)
{
	static const int64_t boot_ms = 0;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - boot_ms;
}

uint32_t k_uptime_get_32(void)
{
	return (uint32_t)k_uptime_get();
}

void k_msleep(int32_t ms)
{
	if (ms > 0) {
		usleep((useconds_t)ms * 1000U);
	}
}

void k_sleep(k_timeout_t timeout)
{
	k_msleep((int32_t)timeout);
}

void k_yield(void)
{
}

/* --------------------------------------------------------------- msgqueue */

int k_msgq_put(struct k_msgq *msgq, const void *data, k_timeout_t timeout)
{
	(void)timeout;
	if (msgq == NULL || data == NULL) {
		return -EINVAL;
	}
	if (msgq->used_msgs >= msgq->max_msgs) {
		return -ENOMSG;
	}
	memcpy(msgq->buffer_start + (size_t)msgq->write_ptr * msgq->msg_size,
		data, msgq->msg_size);
	msgq->write_ptr = (msgq->write_ptr + 1U) % msgq->max_msgs;
	++msgq->used_msgs;
	return 0;
}

int k_msgq_get(struct k_msgq *msgq, void *data, k_timeout_t timeout)
{
	(void)timeout;
	if (msgq == NULL || data == NULL) {
		return -EINVAL;
	}
	if (msgq->used_msgs == 0U) {
		return -ENOMSG;
	}
	memcpy(data, msgq->buffer_start + (size_t)msgq->read_ptr * msgq->msg_size,
		msgq->msg_size);
	msgq->read_ptr = (msgq->read_ptr + 1U) % msgq->max_msgs;
	--msgq->used_msgs;
	return 0;
}

int k_msgq_num_used_get(struct k_msgq *msgq)
{
	return msgq == NULL ? 0 : (int)msgq->used_msgs;
}

/* -------------------------------------------------------------------- rtc */

/* Flipped by the preview harness to exercise the uncalibrated home page. */
int papermono_preview_rtc_ready = 1;

int rtc_get_time(const struct device *dev, struct rtc_time *timeptr)
{
	(void)dev;
	if (timeptr == NULL) {
		return -EINVAL;
	}
	if (!papermono_preview_rtc_ready) {
		return -EINVAL;
	}
	*timeptr = (struct rtc_time) {
		.tm_sec = 0,
		.tm_min = 41,
		.tm_hour = 9,
		.tm_mday = 30,
		.tm_mon = 8,
		.tm_year = 126,
		.tm_wday = 3,
		.tm_yday = 272,
		.tm_isdst = 0,
	};
	return 0;
}

int rtc_set_time(const struct device *dev, const struct rtc_time *timeptr)
{
	(void)dev;
	(void)timeptr;
	return 0;
}

/* ------------------------------------------------------------------- gpio */

static int preview_gpio_level = 1;

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
	(void)spec;
	(void)flags;
	return 0;
}

int gpio_pin_get_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	return preview_gpio_level;
}

int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
	(void)spec;
	preview_gpio_level = value;
	return 0;
}

int gpio_pin_toggle_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	preview_gpio_level = !preview_gpio_level;
	return 0;
}

int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
	(void)spec;
	(void)flags;
	return 0;
}

void gpio_init_callback(struct gpio_callback *callback, void *handler,
	unsigned int pin_mask)
{
	(void)callback;
	(void)handler;
	(void)pin_mask;
}

int gpio_add_callback(const struct device *port, struct gpio_callback *callback)
{
	(void)port;
	(void)callback;
	return 0;
}

FRESULT f_opendir(DIR *directory, const char *path)
{
	struct fs_dir_t fs_dir;
	int ret;

	if (directory == NULL) {
		return FR_INVALID_OBJECT;
	}
	fs_dir_t_init(&fs_dir);
	ret = fs_opendir(&fs_dir, path);
	if (ret < 0) {
		return ret == -ENOENT ? FR_NO_PATH : FR_NO_FILE;
	}
	directory->dir = fs_dir.dir;
	return FR_OK;
}

FRESULT f_readdir(DIR *directory, FILINFO *info)
{
	struct fs_dir_t fs_dir;
	struct fs_dirent entry;
	int ret;

	if (directory == NULL || info == NULL || directory->dir == NULL) {
		return FR_INVALID_OBJECT;
	}
	memset(info, 0, sizeof(*info));
	fs_dir.dir = directory->dir;
	ret = fs_readdir(&fs_dir, &entry);
	directory->dir = fs_dir.dir;
	if (ret < 0) {
		return FR_NO_FILE;
	}
	if (entry.name[0] == '\0') {
		info->fname[0] = '\0';
		return FR_OK;
	}
	snprintf(info->fname, sizeof(info->fname), "%s", entry.name);
	info->fsize = (uint32_t)entry.size;
	info->fattrib = entry.type == FS_DIR_ENTRY_DIR ? AM_DIR : 0U;
	return FR_OK;
}

FRESULT f_closedir(DIR *directory)
{
	struct fs_dir_t fs_dir;

	if (directory == NULL) {
		return FR_INVALID_OBJECT;
	}
	fs_dir.dir = directory->dir;
	(void)fs_closedir(&fs_dir);
	directory->dir = NULL;
	return FR_OK;
}

FRESULT f_stat(const char *path, FILINFO *info)
{
	struct fs_dirent entry;
	int ret;

	if (path == NULL || info == NULL) {
		return FR_INVALID_OBJECT;
	}
	ret = fs_stat(path, &entry);
	if (ret < 0) {
		return FR_NO_FILE;
	}
	memset(info, 0, sizeof(*info));
	snprintf(info->fname, sizeof(info->fname), "%s", entry.name);
	info->fsize = (uint32_t)entry.size;
	info->fattrib = entry.type == FS_DIR_ENTRY_DIR ? AM_DIR : 0U;
	return FR_OK;
}

FRESULT f_unlink(const char *path)
{
	return fs_unlink(path) < 0 ? FR_NO_FILE : FR_OK;
}

/* -------------------------------------------------------------- disk access */

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
	return 0;
}

/* ---------------------------------------------------------------- settings */

/* Minimal in-memory key/blob store with real Zephyr-shaped read callbacks. */

struct settings_entry {
	char key[160];
	uint8_t value[4096];
	size_t length;
	bool used;
};

#define SETTINGS_SLOTS 64U
static struct settings_entry settings_store[SETTINGS_SLOTS];

static struct settings_entry *settings_find(const char *key, bool create)
{
	struct settings_entry *free_slot = NULL;

	for (size_t index = 0U; index < SETTINGS_SLOTS; ++index) {
		if (settings_store[index].used) {
			if (strcmp(settings_store[index].key, key) == 0) {
				return &settings_store[index];
			}
		} else if (free_slot == NULL) {
			free_slot = &settings_store[index];
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	free_slot->used = true;
	snprintf(free_slot->key, sizeof(free_slot->key), "%s", key);
	free_slot->length = 0U;
	return free_slot;
}

static int settings_read_entry(void *cb_arg, void *data, size_t length)
{
	const struct settings_entry *entry = cb_arg;
	size_t copied;

	if (entry == NULL) {
		return -EINVAL;
	}
	copied = length < entry->length ? length : entry->length;
	if (data != NULL && copied > 0U) {
		memcpy(data, entry->value, copied);
	}
	return (int)copied;
}

/* Split "subtree/leaf" against "subtree": returns true and fills *leaf. */
static bool settings_leaf(const char *full_key, const char *subtree,
	const char **leaf)
{
	const size_t length = strlen(subtree);

	if (strcmp(full_key, subtree) == 0) {
		*leaf = "";
		return true;
	}
	if (strncmp(full_key, subtree, length) == 0 && full_key[length] == '/') {
		*leaf = full_key + length + 1U;
		return true;
	}
	return false;
}

int settings_subsys_init(void)
{
	return 0;
}

int settings_load(void)
{
	return 0;
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	struct settings_entry *entry;

	if (key == NULL || length > sizeof(entry->value)) {
		return -EINVAL;
	}
	entry = settings_find(key, true);
	if (entry == NULL) {
		return -ENOMEM;
	}
	if (value != NULL && length > 0U) {
		memcpy(entry->value, value, length);
	}
	entry->length = length;
	return 0;
}

int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param)
{
	int matched = 0;
	int last_error = 0;

	if (subtree == NULL || callback == NULL) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < SETTINGS_SLOTS; ++index) {
		struct settings_entry *entry = &settings_store[index];
		const char *leaf = NULL;
		int ret;

		if (!entry->used || !settings_leaf(entry->key, subtree, &leaf)) {
			continue;
		}
		++matched;
		/* Zephyr passes the VALUE length here, not the key length:
		 * settings_line.c does `len = settings_line_val_get_len(...)`
		 * before calling the handler/callback.
		 */
		ret = callback(leaf, entry->length, settings_read_entry, entry,
			param);
		if (ret < 0) {
			last_error = ret;
		}
	}
	if (matched == 0) {
		return -ENOENT;
	}
	return last_error;
}

/*
 * SETTINGS_STATIC_HANDLER_DEFINE() in the portable modules emits
 * settings_host_set_<name>() / settings_host_commit_<name>().  Dispatch to the
 * ones this preview actually needs; everything else is a no-op.
 */
extern int settings_host_set_papermono(const char *key, size_t len,
	settings_read_cb read_cb, void *cb_arg) __attribute__((weak));
extern int settings_host_commit_papermono(void) __attribute__((weak));

int settings_load_subtree(const char *subtree)
{
	int ret = 0;

	if (subtree == NULL) {
		return -EINVAL;
	}
	if (strcmp(subtree, "papermono") == 0) {
		if (&settings_host_set_papermono == NULL) {
			return 0;
		}
		for (size_t index = 0U; index < SETTINGS_SLOTS; ++index) {
			struct settings_entry *entry = &settings_store[index];
			const char *leaf = NULL;

			if (!entry->used ||
			    !settings_leaf(entry->key, subtree, &leaf)) {
				continue;
			}
			(void)settings_host_set_papermono(leaf, entry->length,
				settings_read_entry, entry);
		}
		if (&settings_host_commit_papermono != NULL) {
			ret = settings_host_commit_papermono();
		}
	}
	return ret;
}

int settings_delete(const char *key)
{
	struct settings_entry *entry = settings_find(key, false);

	if (entry == NULL) {
		return -ENOENT;
	}
	entry->used = false;
	entry->length = 0U;
	return 0;
}

int settings_commit_subtree(const char *subtree)
{
	(void)subtree;
	return 0;
}
