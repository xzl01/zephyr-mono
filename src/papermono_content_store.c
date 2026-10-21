/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <ff.h>

#include <papermono/content_store.h>
#include <papermono/content_validation.h>
#include <papermono/text_codec.h>

LOG_MODULE_REGISTER(papermono_content_store, LOG_LEVEL_INF);

#define PAPERMONO_DISK_NAME "SD"
#define PAPERMONO_UPLOAD_DIRECTORY ".upload"
#define PAPERMONO_BACKUP_DIRECTORY ".backup"

static FATFS fat_fs;
static struct fs_mount_t mount = {
	.type = FS_FATFS,
	.mnt_point = PAPERMONO_SD_MOUNT_POINT,
	.fs_data = &fat_fs,
	.storage_dev = (void *)PAPERMONO_DISK_NAME,
	.flags = FS_MOUNT_FLAG_NO_FORMAT | FS_MOUNT_FLAG_USE_DISK_ACCESS,
};
static enum papermono_media_state media_state =
	PAPERMONO_MEDIA_UNINITIALIZED;
static bool mounted;
static struct papermono_content_transaction *active_import;
static bool detect_configured;
static const struct gpio_dt_spec card_detect =
	GPIO_DT_SPEC_GET(DT_NODELABEL(sd_slot_control), detect_gpios);
K_MUTEX_DEFINE(content_lock);

static int card_present(void)
{
	if (!detect_configured) {
		return -EACCES;
	}
	return gpio_pin_get_dt(&card_detect);
}

static bool ascii_equal_ci(char left, char right)
{
	if (left >= 'A' && left <= 'Z') {
		left += 'a' - 'A';
	}
	if (right >= 'A' && right <= 'Z') {
		right += 'a' - 'A';
	}
	return left == right;
}

static bool has_extension(const char *name, const char *extension)
{
	const size_t name_length = strlen(name);
	const size_t extension_length = strlen(extension);

	if (name_length <= extension_length) {
		return false;
	}
	for (size_t index = 0U; index < extension_length; ++index) {
		if (!ascii_equal_ci(name[name_length - extension_length + index],
				extension[index])) {
			return false;
		}
	}
	return true;
}

static enum papermono_content_kind book_kind(const char *name)
{
	if (has_extension(name, ".txt")) {
		return PAPERMONO_CONTENT_TXT;
	}
	if (has_extension(name, ".epub")) {
		return PAPERMONO_CONTENT_EPUB;
	}
	return PAPERMONO_CONTENT_UNKNOWN;
}

static enum papermono_content_kind directory_kind(const char *directory,
	const char *name)
{
	if (strcmp(directory, PAPERMONO_BOOKS_DIRECTORY) == 0) {
		return book_kind(name);
	}
	if (strcmp(directory, PAPERMONO_CALENDAR_DIRECTORY) == 0) {
		return has_extension(name, ".ics") ? PAPERMONO_CONTENT_ICS :
			PAPERMONO_CONTENT_UNKNOWN;
	}
	if (strcmp(directory, PAPERMONO_TASKS_DIRECTORY) == 0) {
		return strcmp(name, "tasks-v1.json") == 0 ?
			PAPERMONO_CONTENT_TASKS : PAPERMONO_CONTENT_UNKNOWN;
	}
	if (strcmp(directory, PAPERMONO_NOTES_DIRECTORY) == 0) {
		return has_extension(name, ".md") || has_extension(name, ".txt") ?
			PAPERMONO_CONTENT_NOTE : PAPERMONO_CONTENT_UNKNOWN;
	}
	if (strcmp(directory, PAPERMONO_WEREAD_DIRECTORY) == 0) {
		return strcmp(name, PAPERMONO_WEREAD_CATALOG_NAME) == 0 ?
			PAPERMONO_CONTENT_WEREAD : PAPERMONO_CONTENT_UNKNOWN;
	}
	return PAPERMONO_CONTENT_UNKNOWN;
}

static size_t title_extension_length(enum papermono_content_kind kind,
	const char *name)
{
	switch (kind) {
	case PAPERMONO_CONTENT_TXT:
	case PAPERMONO_CONTENT_ICS:
		return 4U;
	case PAPERMONO_CONTENT_EPUB:
		return 5U;
	case PAPERMONO_CONTENT_NOTE:
		return has_extension(name, ".md") ? 3U : 4U;
	case PAPERMONO_CONTENT_TASKS:
	case PAPERMONO_CONTENT_WEREAD:
	case PAPERMONO_CONTENT_UNKNOWN:
	default:
		return 0U;
	}
}

static const char *kind_directory(enum papermono_content_kind kind)
{
	switch (kind) {
	case PAPERMONO_CONTENT_TXT:
	case PAPERMONO_CONTENT_EPUB:
		return PAPERMONO_BOOKS_DIRECTORY;
	case PAPERMONO_CONTENT_ICS:
		return PAPERMONO_CALENDAR_DIRECTORY;
	case PAPERMONO_CONTENT_TASKS:
		return PAPERMONO_TASKS_DIRECTORY;
	case PAPERMONO_CONTENT_NOTE:
		return PAPERMONO_NOTES_DIRECTORY;
	case PAPERMONO_CONTENT_WEREAD:
		return PAPERMONO_WEREAD_DIRECTORY;
	case PAPERMONO_CONTENT_UNKNOWN:
	default:
		return NULL;
	}
}

static uint64_t kind_file_limit(enum papermono_content_kind kind)
{
	if (kind == PAPERMONO_CONTENT_WEREAD) {
		return PAPERMONO_WEREAD_FILE_LIMIT;
	}
	return kind == PAPERMONO_CONTENT_TXT || kind == PAPERMONO_CONTENT_EPUB ?
		PAPERMONO_BOOK_FILE_LIMIT : PAPERMONO_METADATA_FILE_LIMIT;
}

static bool kind_matches_name(enum papermono_content_kind kind,
	const char *name)
{
	switch (kind) {
	case PAPERMONO_CONTENT_TXT:
		return has_extension(name, ".txt");
	case PAPERMONO_CONTENT_EPUB:
		return has_extension(name, ".epub");
	case PAPERMONO_CONTENT_ICS:
		return has_extension(name, ".ics");
	case PAPERMONO_CONTENT_TASKS:
		return strcmp(name, "tasks-v1.json") == 0;
	case PAPERMONO_CONTENT_NOTE:
		return has_extension(name, ".md") || has_extension(name, ".txt");
	case PAPERMONO_CONTENT_WEREAD:
		return strcmp(name, PAPERMONO_WEREAD_CATALOG_NAME) == 0;
	case PAPERMONO_CONTENT_UNKNOWN:
	default:
		return false;
	}
}

static uint64_t stable_hash(const char *path, uint64_t size, uint32_t modified)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	const uint8_t *bytes = (const uint8_t *)path;

	while (*bytes != 0U) {
		hash = (hash ^ *bytes++) * UINT64_C(1099511628211);
	}
	for (size_t index = 0U; index < sizeof(size); ++index) {
		hash = (hash ^ (uint8_t)(size >> (index * 8U))) *
			UINT64_C(1099511628211);
	}
	for (size_t index = 0U; index < sizeof(modified); ++index) {
		hash = (hash ^ (uint8_t)(modified >> (index * 8U))) *
			UINT64_C(1099511628211);
	}
	return hash;
}

static int ensure_directory(const char *path)
{
	struct fs_dirent entry;
	int ret = fs_stat(path, &entry);

	/* Probe first: fs_mkdir() logs an error for an already existing directory
	 * before returning -EEXIST, and the store creates its directory tree on
	 * every mount, so calling it blindly floods the console with
	 * "failed to create directory (-17)" on every boot.
	 */
	if (ret == 0) {
		return entry.type == FS_DIR_ENTRY_DIR ? 0 : -ENOTDIR;
	}
	if (ret != -ENOENT) {
		return ret;
	}
	ret = fs_mkdir(path);
	return ret == -EEXIST ? 0 : ret;
}

/* Remove a file only when it is actually there. fs_unlink() reports -ENOENT
 * and Zephyr logs it at error level, so calling it unconditionally turns normal
 * cleanup of an absent backup or upload artifact into console errors.
 */
static void unlink_if_present(const char *path)
{
	struct fs_dirent entry;

	if (fs_stat(path, &entry) == 0) {
		(void)fs_unlink(path);
	}
}

static int transaction_directory_path(char *buffer, size_t capacity,
	const char *directory, const char *transaction_directory)
{
	const int length = snprintf(buffer, capacity, "%s/%s", directory,
		transaction_directory);

	return length < 0 || length >= (int)capacity ? -ENAMETOOLONG : 0;
}

static int ensure_transaction_directories(const char *path)
{
	char transaction_path[PAPERMONO_CONTENT_PATH_MAX];
	int ret;

	ret = transaction_directory_path(transaction_path,
		sizeof(transaction_path), path, PAPERMONO_UPLOAD_DIRECTORY);
	ret = ret ?: ensure_directory(transaction_path);
	ret = ret ?: transaction_directory_path(transaction_path,
		sizeof(transaction_path), path, PAPERMONO_BACKUP_DIRECTORY);
	return ret ?: ensure_directory(transaction_path);
}

static int recover_legacy_directory(const char *path)
{
	struct fs_dir_t directory;
	struct fs_dirent entry;
	int ret;

	fs_dir_t_init(&directory);
	ret = fs_opendir(&directory, path);
	if (ret < 0) {
		return ret;
	}
	for (;;) {
		char artifact[PAPERMONO_CONTENT_PATH_MAX];
		char final[PAPERMONO_CONTENT_PATH_MAX];
		struct fs_dirent final_entry;
		const char *suffix;
		size_t base_length;

		ret = fs_readdir(&directory, &entry);
		if (ret < 0 || entry.name[0] == '\0') {
			break;
		}
		if (entry.type != FS_DIR_ENTRY_FILE || entry.name[0] != '.') {
			continue;
		}
		suffix = has_extension(entry.name, ".upload") ? ".upload" :
			has_extension(entry.name, ".backup") ? ".backup" : NULL;
		if (suffix == NULL) {
			continue;
		}
		base_length = strlen(entry.name) - strlen(suffix);
		if (base_length <= 1U) {
			continue;
		}
		if (snprintf(artifact, sizeof(artifact), "%s/%s", path,
			entry.name) >= (int)sizeof(artifact) ||
		    snprintf(final, sizeof(final), "%s/%.*s", path,
			(int)(base_length - 1U), entry.name + 1U) >=
				(int)sizeof(final)) {
			continue;
		}
		if (strcmp(suffix, ".upload") == 0) {
			(void)fs_unlink(artifact);
			continue;
		}
		if (fs_stat(final, &final_entry) == 0) {
			(void)fs_unlink(artifact);
		} else {
			(void)fs_rename(artifact, final);
		}
	}
	(void)fs_closedir(&directory);
	return ret;
}

static int recover_transaction_directory(const char *path,
	const char *transaction_directory, bool restore)
{
	char transaction_path[PAPERMONO_CONTENT_PATH_MAX];
	struct fs_dir_t directory;
	struct fs_dirent entry;
	int ret;

	ret = transaction_directory_path(transaction_path,
		sizeof(transaction_path), path, transaction_directory);
	if (ret < 0) {
		return ret;
	}
	fs_dir_t_init(&directory);
	ret = fs_opendir(&directory, transaction_path);
	if (ret < 0) {
		return ret;
	}
	for (;;) {
		char artifact[PAPERMONO_CONTENT_PATH_MAX];
		char final[PAPERMONO_CONTENT_PATH_MAX];
		struct fs_dirent final_entry;

		ret = fs_readdir(&directory, &entry);
		if (ret < 0 || entry.name[0] == '\0') {
			break;
		}
		if (entry.type != FS_DIR_ENTRY_FILE ||
		    !papermono_content_store_safe_name(entry.name) ||
		    snprintf(artifact, sizeof(artifact), "%s/%s",
			transaction_path, entry.name) >= (int)sizeof(artifact) ||
		    snprintf(final, sizeof(final), "%s/%s", path,
			entry.name) >= (int)sizeof(final)) {
			continue;
		}
		if (!restore || fs_stat(final, &final_entry) == 0) {
			(void)fs_unlink(artifact);
		} else {
			(void)fs_rename(artifact, final);
		}
	}
	(void)fs_closedir(&directory);
	return ret;
}

static int recover_directory(const char *path)
{
	int ret;

	/* First consume the pre-directory transaction format so updating firmware
	 * never strands an older .<name>.upload/.backup artifact.
	 */
	ret = recover_legacy_directory(path);
	ret = ret ?: recover_transaction_directory(path,
		PAPERMONO_UPLOAD_DIRECTORY, false);
	return ret ?: recover_transaction_directory(path,
		PAPERMONO_BACKUP_DIRECTORY, true);
}

bool papermono_content_store_safe_name(const char *name)
{
	size_t length;

	if (name == NULL) {
		return false;
	}
	length = strnlen(name, MAX_FILE_NAME + 1U);
	if (length == 0U || length > MAX_FILE_NAME || name[0] == '.' ||
	    strstr(name, "..") != NULL ||
	    !papermono_text_utf8_valid((const uint8_t *)name, length, false)) {
		return false;
	}
	for (size_t index = 0U; index < length; ++index) {
		const unsigned char value = (unsigned char)name[index];

		if (value < 0x20U || name[index] == '/' || name[index] == '\\' ||
		    name[index] == ':') {
			return false;
		}
	}
	return true;
}

static bool content_entry_path_valid(
	const struct papermono_content_entry *entry, const char *directory)
{
	const size_t directory_length = directory == NULL ? 0U : strlen(directory);
	const char *name;

	if (entry == NULL || directory == NULL ||
	    strncmp(entry->path, directory, directory_length) != 0 ||
	    entry->path[directory_length] != '/') {
		return false;
	}
	name = entry->path + directory_length + 1U;
	return papermono_content_store_safe_name(name) &&
		kind_matches_name(entry->kind, name);
}

/* An import is validated while it is still in the staging directory, so the
 * EPUB reader has to be able to open "<dir>/.upload/<name>". The directory
 * name itself starts with a dot and contains no slash, which is exactly what
 * papermono_content_store_safe_name() rejects, so the staging shape gets its
 * own check: the leaf name must still satisfy every rule a published file
 * does, and the staging directory must be the only extra component.
 */
static bool content_upload_path_valid(
	const struct papermono_content_entry *entry, const char *directory)
{
	const size_t directory_length = directory == NULL ? 0U : strlen(directory);
	const size_t prefix_length =
		sizeof(PAPERMONO_UPLOAD_DIRECTORY "/") - 1U;
	const char *name;

	if (entry == NULL || directory == NULL ||
	    (size_t)directory_length == 0U ||
	    strncmp(entry->path, directory, directory_length) != 0 ||
	    entry->path[directory_length] != '/' ||
	    strncmp(entry->path + directory_length + 1U,
		    PAPERMONO_UPLOAD_DIRECTORY "/", prefix_length) != 0) {
		return false;
	}
	name = entry->path + directory_length + 1U + prefix_length;
	return papermono_content_store_safe_name(name) &&
		kind_matches_name(entry->kind, name);
}

int papermono_content_store_mount(void)
{
	const int present = card_present();
	int ret;

	if (present <= 0) {
		media_state = present == 0 ? PAPERMONO_MEDIA_REMOVED :
			PAPERMONO_MEDIA_ERROR;
		return present == 0 ? -ENODEV : present;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (mounted) {
		k_mutex_unlock(&content_lock);
		return 0;
	}
	ret = disk_access_ioctl(PAPERMONO_DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret < 0) {
		media_state = ret == -ENODEV ?
			PAPERMONO_MEDIA_REMOVED : PAPERMONO_MEDIA_ERROR;
		goto out;
	}
	ret = fs_mount(&mount);
	if (ret < 0) {
		/* The card answered at the SD layer (presence was checked above), so
		 * a mount failure here means the volume carries no filesystem Elm
		 * FATFS understands. -ENODEV is what fat_fs.c returns for
		 * FR_NO_FILESYSTEM / FR_INVALID_DRIVE / FR_NOT_ENABLED, which is the
		 * normal state of a card that shipped exFAT or GPT-partitioned.
		 */
		media_state = ret == -ENODEV || ret == -EINVAL ||
			ret == -ENOTSUP ? PAPERMONO_MEDIA_UNFORMATTED :
			PAPERMONO_MEDIA_ERROR;
		goto deinit;
	}
	mounted = true;
	ret = ensure_directory(PAPERMONO_CONTENT_ROOT);
	ret = ret ?: ensure_directory(PAPERMONO_BOOKS_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_CALENDAR_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_TASKS_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_NOTES_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_WEREAD_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_CACHE_DIRECTORY);
	ret = ret ?: ensure_directory(PAPERMONO_READER_CACHE_DIRECTORY);
	ret = ret ?: ensure_transaction_directories(PAPERMONO_BOOKS_DIRECTORY);
	ret = ret ?: ensure_transaction_directories(PAPERMONO_CALENDAR_DIRECTORY);
	ret = ret ?: ensure_transaction_directories(PAPERMONO_TASKS_DIRECTORY);
	ret = ret ?: ensure_transaction_directories(PAPERMONO_NOTES_DIRECTORY);
	ret = ret ?: ensure_transaction_directories(PAPERMONO_WEREAD_DIRECTORY);
	if (ret < 0) {
		media_state = PAPERMONO_MEDIA_ERROR;
		(void)fs_unmount(&mount);
		mounted = false;
		goto deinit;
	}
	ret = recover_directory(PAPERMONO_BOOKS_DIRECTORY);
	ret = ret ?: recover_directory(PAPERMONO_CALENDAR_DIRECTORY);
	ret = ret ?: recover_directory(PAPERMONO_TASKS_DIRECTORY);
	ret = ret ?: recover_directory(PAPERMONO_NOTES_DIRECTORY);
	ret = ret ?: recover_directory(PAPERMONO_WEREAD_DIRECTORY);
	if (ret < 0) {
		media_state = PAPERMONO_MEDIA_ERROR;
		(void)fs_unmount(&mount);
		mounted = false;
		goto deinit;
	}
	media_state = PAPERMONO_MEDIA_READY;
	LOG_INF("TF content store mounted at %s", PAPERMONO_SD_MOUNT_POINT);
	goto out;

deinit:
	(void)disk_access_ioctl(PAPERMONO_DISK_NAME,
		DISK_IOCTL_CTRL_DEINIT, NULL);
out:
	k_mutex_unlock(&content_lock);
	return ret;
}

int papermono_content_store_format(void)
{
	int ret;

#if defined(CONFIG_PAPERMONO_HIL_FAKE_UNFORMATTED)
	/* Acceptance walkthrough only: never erase the mounted card. */
	LOG_WRN("HIL: simulated TF format (card contents preserved)");
	k_mutex_lock(&content_lock, K_FOREVER);
	media_state = PAPERMONO_MEDIA_READY;
	k_mutex_unlock(&content_lock);
	return 0;
#endif

	if (card_present() <= 0) {
		return -ENODEV;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (mounted) {
		/* Formatting a mounted volume would corrupt every open handle. */
		k_mutex_unlock(&content_lock);
		return -EBUSY;
	}
	ret = disk_access_ioctl(PAPERMONO_DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
	if (ret < 0) {
		goto out;
	}
	/* Super-floppy FAT (no MBR): FM_SFD. Elm FATFS mounts that directly, and
	 * it is the only layout this firmware can read. This erases the card.
	 */
	ret = fs_mkfs(FS_FATFS, (uintptr_t)PAPERMONO_DISK_NAME, NULL, 0);
	if (ret < 0) {
		LOG_ERR("TF format failed (%d)", ret);
		(void)disk_access_ioctl(PAPERMONO_DISK_NAME,
			DISK_IOCTL_CTRL_DEINIT, NULL);
		goto out;
	}
	media_state = PAPERMONO_MEDIA_UNFORMATTED;
	LOG_WRN("TF card formatted; all previous content was erased");
out:
	k_mutex_unlock(&content_lock);
	if (ret < 0) {
		return ret;
	}
	return papermono_content_store_mount();
}

int papermono_content_store_unmount(void)
{
	int ret = 0;

	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != NULL) {
		if (active_import->file_open) {
			(void)fs_close(&active_import->file);
			active_import->file_open = false;
		}
		(void)fs_unlink(active_import->upload_path);
		active_import->active = false;
		active_import = NULL;
		LOG_WRN("Active content import cancelled while unmounting TF card");
	}
	if (mounted) {
		ret = fs_unmount(&mount);
		if (ret == 0) {
			mounted = false;
		}
	}
	if (!mounted) {
		(void)disk_access_ioctl(PAPERMONO_DISK_NAME,
			DISK_IOCTL_CTRL_DEINIT, NULL);
		media_state = PAPERMONO_MEDIA_REMOVED;
	}
	k_mutex_unlock(&content_lock);
	return ret;
}

int papermono_content_store_init(void)
{
	int ret;

	media_state = PAPERMONO_MEDIA_UNINITIALIZED;
	if (!gpio_is_ready_dt(&card_detect)) {
		media_state = PAPERMONO_MEDIA_ERROR;
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&card_detect, GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		media_state = PAPERMONO_MEDIA_ERROR;
		return ret;
	}
	detect_configured = true;
	LOG_INF("TF detect initialized: inserted=%d", card_present());
	ret = papermono_content_store_mount();
#if defined(CONFIG_PAPERMONO_TF_FORMAT_WHEN_UNFORMATTED)
	/* Opt-in, destructive: HIL verification with a scratch card only. */
	if (ret < 0 && media_state == PAPERMONO_MEDIA_UNFORMATTED) {
		LOG_WRN("TF card has no usable filesystem; formatting (opt-in)");
		ret = papermono_content_store_format();
	}
#endif
	return ret;
}

int papermono_content_store_poll(void)
{
	const int present = card_present();

	if (present <= 0) {
		if (mounted) {
			(void)papermono_content_store_unmount();
		} else {
			media_state = present == 0 ? PAPERMONO_MEDIA_REMOVED :
				PAPERMONO_MEDIA_ERROR;
		}
		return present == 0 ? -ENODEV : present;
	}
	const int status = disk_access_status(PAPERMONO_DISK_NAME);

	if (status < 0 || (status & DISK_STATUS_NOMEDIA) != 0) {
		if (mounted) {
			(void)papermono_content_store_unmount();
		} else {
			media_state = PAPERMONO_MEDIA_REMOVED;
		}
		return -ENODEV;
	}
	if (!mounted || (status & DISK_STATUS_UNINIT) != 0) {
		return papermono_content_store_mount();
	}
	return 0;
}

enum papermono_media_state papermono_content_store_media_state(void)
{
	enum papermono_media_state state;

#if defined(CONFIG_PAPERMONO_HIL_FAKE_UNFORMATTED)
	/* Acceptance walkthrough only: expose the format entry on a healthy card. */
	return PAPERMONO_MEDIA_UNFORMATTED;
#endif
	k_mutex_lock(&content_lock, K_FOREVER);
	state = media_state;
	k_mutex_unlock(&content_lock);
	return state;
}

const char *papermono_content_store_state_text(enum papermono_media_state state)
{
	switch (state) {
	case PAPERMONO_MEDIA_READY:
		return "TF 卡已就绪";
	case PAPERMONO_MEDIA_EMPTY:
		return "书库为空";
	case PAPERMONO_MEDIA_REMOVED:
		return "未检测到 TF 卡";
	case PAPERMONO_MEDIA_UNFORMATTED:
		return "TF 卡不是受支持的 FAT 文件系统";
	case PAPERMONO_MEDIA_ERROR:
		return "TF 卡访问失败";
	case PAPERMONO_MEDIA_UNINITIALIZED:
	default:
		return "TF 卡尚未初始化";
	}
}

static int scan_directory_locked(const char *path,
	papermono_content_scan_cb callback, void *context, size_t *count)
{
	/* Enumerate with FatFs directly rather than through the Zephyr VFS.
	 * Zephyr's fs_dirent carries no timestamp, which forced a per-file
	 * f_stat() lookup; FatFs resolves each of those by walking the
	 * directory from its first entry, making a full scan quadratic in the
	 * number of files. f_readdir already returns fdate/ftime, so the
	 * timestamps are free and the scan stays linear.
	 */
	DIR directory;
	FILINFO info;
	const char *fat_path = path[0] == '/' ? path + 1 : path;
	size_t found = 0U;
	int ret = 0;

	memset(&info, 0, sizeof(info));
	if (f_opendir(&directory, fat_path) != FR_OK) {
		return -EIO;
	}
	for (;;) {
		struct papermono_content_entry entry;
		enum papermono_content_kind kind;
		uint64_t hash;
		size_t title_length;
		FRESULT result;

		result = f_readdir(&directory, &info);
		if (result != FR_OK) {
			ret = -EIO;
			break;
		}
		if (info.fname[0] == '\0') {
			break;
		}
		if ((info.fattrib & AM_DIR) != 0U ||
		    !papermono_content_store_safe_name(info.fname)) {
			continue;
		}
		kind = directory_kind(path, info.fname);
		if (kind == PAPERMONO_CONTENT_UNKNOWN ||
		    (uint64_t)info.fsize > kind_file_limit(kind)) {
			continue;
		}
		memset(&entry, 0, sizeof(entry));
		entry.kind = kind;
		entry.size = info.fsize;
		ret = snprintf(entry.path, sizeof(entry.path), "%s/%s", path,
			info.fname);
		if (ret < 0 || ret >= (int)sizeof(entry.path)) {
			continue;
		}
		entry.modified = ((uint32_t)info.fdate << 16) |
			(uint32_t)info.ftime;
		title_length = strlen(info.fname) -
			title_extension_length(kind, info.fname);
		title_length = MIN(title_length, sizeof(entry.title) - 1U);
		memcpy(entry.title, info.fname, title_length);
		hash = stable_hash(entry.path, entry.size, entry.modified);
		(void)snprintf(entry.id, sizeof(entry.id), "%016llx",
			(unsigned long long)hash);
		ret = callback(&entry, context);
		if (ret != 0) {
			break;
		}
		++found;
	}
	(void)f_closedir(&directory);
	if (count != NULL) {
		*count = found;
	}
	return ret;
}

int papermono_content_store_scan_books(papermono_content_scan_cb callback,
	void *context, size_t *count)
{
	size_t found = 0U;
	int ret;

	if (callback == NULL) {
		return -EINVAL;
	}
	ret = papermono_content_store_poll();
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	ret = scan_directory_locked(PAPERMONO_BOOKS_DIRECTORY, callback,
		context, &found);
	if (ret == 0 && found == 0U) {
		media_state = PAPERMONO_MEDIA_EMPTY;
	} else if (ret == 0) {
		media_state = PAPERMONO_MEDIA_READY;
	}
	k_mutex_unlock(&content_lock);
	if (count != NULL) {
		*count = found;
	}
	return ret;
}

int papermono_content_store_scan_all(papermono_content_scan_cb callback,
	void *context, size_t *count)
{
	static const char *const directories[] = {
		PAPERMONO_BOOKS_DIRECTORY,
		PAPERMONO_CALENDAR_DIRECTORY,
		PAPERMONO_TASKS_DIRECTORY,
		PAPERMONO_NOTES_DIRECTORY,
		PAPERMONO_WEREAD_DIRECTORY,
	};
	size_t total = 0U;
	int ret;

	if (callback == NULL) {
		return -EINVAL;
	}
	ret = papermono_content_store_poll();
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	for (size_t index = 0U; index < ARRAY_SIZE(directories); ++index) {
		size_t found = 0U;

		ret = scan_directory_locked(directories[index], callback, context,
			&found);
		total += found;
		if (ret != 0) {
			break;
		}
	}
	k_mutex_unlock(&content_lock);
	if (count != NULL) {
		*count = total;
	}
	return ret;
}

int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file)
{
	const char *directory = entry == NULL ? NULL : kind_directory(entry->kind);

	/* Import validation reads the file back from the staging directory, so
	 * both the published and the not-yet-published location are openable.
	 * Neither form can escape the content root: the leaf name is checked by
	 * safe_name() and by the kind's expected filename in both cases.
	 */
	if (file == NULL ||
	    !(content_entry_path_valid(entry, directory) ||
	      content_upload_path_valid(entry, directory))) {
		return -EINVAL;
	}
	if (papermono_content_store_poll() < 0) {
		return -ENODEV;
	}
	fs_file_t_init(file);
	return fs_open(file, entry->path, FS_O_READ);
}

int papermono_content_store_import_begin(
	struct papermono_content_transaction *transaction,
	enum papermono_content_kind kind, const char *name, uint64_t size,
	bool verify_crc32, uint32_t expected_crc32)
{
	const char *directory = kind_directory(kind);
	struct fs_statvfs capacity;
	const size_t name_length = name == NULL ? 0U : strlen(name);
	const uint64_t limit = kind_file_limit(kind);
	int ret;

	if (transaction == NULL || directory == NULL ||
	    !papermono_content_store_safe_name(name) ||
	    !kind_matches_name(kind, name) || name_length > MAX_FILE_NAME ||
	    size > limit) {
		return -EINVAL;
	}
	ret = papermono_content_store_poll();
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != NULL) {
		ret = -EBUSY;
		goto out;
	}
	ret = fs_statvfs(directory, &capacity);
	if (ret < 0) {
		goto out;
	}
	if (size > (uint64_t)capacity.f_bsize * capacity.f_bfree) {
		ret = -ENOSPC;
		goto out;
	}
	memset(transaction, 0, sizeof(*transaction));
	if (snprintf(transaction->final_path,
			sizeof(transaction->final_path), "%s/%s", directory, name) >=
		    (int)sizeof(transaction->final_path) ||
	    snprintf(transaction->upload_path,
			sizeof(transaction->upload_path), "%s/%s/%s", directory,
			PAPERMONO_UPLOAD_DIRECTORY, name) >=
			(int)sizeof(transaction->upload_path) ||
	    snprintf(transaction->backup_path,
			sizeof(transaction->backup_path), "%s/%s/%s", directory,
			PAPERMONO_BACKUP_DIRECTORY, name) >=
			(int)sizeof(transaction->backup_path)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	/* A leftover upload artifact only exists after an interrupted import, so
	 * probe first: the unconditional unlink logged an error on every first
	 * upload of a new file.
	 */
	unlink_if_present(transaction->upload_path);
	fs_file_t_init(&transaction->file);
	ret = fs_open(&transaction->file, transaction->upload_path,
		FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		goto out;
	}
	transaction->expected_size = size;
	transaction->expected_crc32 = expected_crc32;
	transaction->kind = kind;
	transaction->verify_crc32 = verify_crc32;
	transaction->active = true;
	transaction->file_open = true;
	active_import = transaction;
out:
	k_mutex_unlock(&content_lock);
	return ret;
}

int papermono_content_store_import_write(
	struct papermono_content_transaction *transaction,
	const uint8_t *data, size_t length)
{
	ssize_t written;

	if (transaction == NULL || (data == NULL && length > 0U)) {
		return -EINVAL;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != transaction || !transaction->active ||
	    !transaction->file_open) {
		k_mutex_unlock(&content_lock);
		return -ENODEV;
	}
	if (length > transaction->expected_size - transaction->written) {
		k_mutex_unlock(&content_lock);
		return -EINVAL;
	}
	written = fs_write(&transaction->file, data, length);
	if (written == (ssize_t)length) {
		transaction->crc32 = crc32_ieee_update(transaction->crc32,
			data, length);
		transaction->written += length;
	}
	k_mutex_unlock(&content_lock);
	return written < 0 ? (int)written :
		written == (ssize_t)length ? 0 : -EIO;
}

int papermono_content_store_import_abort(
	struct papermono_content_transaction *transaction)
{
	if (transaction == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != transaction || !transaction->active) {
		k_mutex_unlock(&content_lock);
		return -ENODEV;
	}
	if (transaction->file_open) {
		(void)fs_close(&transaction->file);
		transaction->file_open = false;
	}
	(void)fs_unlink(transaction->upload_path);
	transaction->active = false;
	active_import = NULL;
	k_mutex_unlock(&content_lock);
	return 0;
}

int papermono_content_store_import_commit(
	struct papermono_content_transaction *transaction)
{
	struct fs_dirent existing;
	bool has_backup = false;
	int ret;

	if (transaction == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != transaction || !transaction->active ||
	    !transaction->file_open) {
		k_mutex_unlock(&content_lock);
		return -ENODEV;
	}
	if (transaction->written != transaction->expected_size ||
	    (transaction->verify_crc32 &&
	     transaction->crc32 != transaction->expected_crc32)) {
		ret = -EBADMSG;
		goto abort;
	}
	ret = fs_sync(&transaction->file);
	const int close_ret = fs_close(&transaction->file);

	transaction->file_open = false;
	if (ret == 0) {
		ret = close_ret;
	}
	if (ret < 0) {
		goto unlink_upload;
	}
	/* Validation may mount/poll the medium through the EPUB reader. Do not hold
	 * the store mutex across that call; import_active still excludes another
	 * transaction and the final name is untouched until validation succeeds.
	 */
	k_mutex_unlock(&content_lock);
	ret = papermono_content_validate(transaction->kind,
		transaction->upload_path, transaction->written);
	k_mutex_lock(&content_lock, K_FOREVER);
	if (active_import != transaction || !transaction->active) {
		k_mutex_unlock(&content_lock);
		return -ENODEV;
	}
	if (ret < 0) {
		goto unlink_upload;
	}
	unlink_if_present(transaction->backup_path);
	int stat_ret = fs_stat(transaction->final_path, &existing);

	if (stat_ret == 0) {
		ret = fs_rename(transaction->final_path, transaction->backup_path);
		if (ret < 0) {
			goto unlink_upload;
		}
		has_backup = true;
	} else if (stat_ret != -ENOENT) {
		/* Cannot tell whether a previous file exists. Overwriting blind could
		 * destroy it with no backup, so refuse the commit.
		 */
		ret = stat_ret;
		goto unlink_upload;
	}
	ret = fs_rename(transaction->upload_path, transaction->final_path);
	if (ret < 0) {
		if (has_backup) {
			(void)fs_rename(transaction->backup_path,
				transaction->final_path);
		}
		goto finish;
	}
	/* Confirm the renamed file is visible with the expected size. This layer
	 * does not sync the directory entry, so a failing medium can leave the name
	 * missing or short; reporting success then would tell the client the upload
	 * landed when it did not. Restore the previous file when there is one.
	 */
	struct fs_dirent committed;

	if (fs_stat(transaction->final_path, &committed) < 0 ||
	    committed.size != (size_t)transaction->written) {
		LOG_ERR("Committed content failed verification (size %u, expected %u)",
			(unsigned int)committed.size,
			(unsigned int)transaction->written);
		if (has_backup) {
			(void)fs_unlink(transaction->final_path);
			(void)fs_rename(transaction->backup_path,
				transaction->final_path);
		}
		ret = -EIO;
		goto finish;
	}
	if (has_backup) {
		(void)fs_unlink(transaction->backup_path);
	}
	goto finish;

abort:
	if (transaction->file_open) {
		(void)fs_close(&transaction->file);
		transaction->file_open = false;
	}
unlink_upload:
	(void)fs_unlink(transaction->upload_path);
finish:
	transaction->active = false;
	if (active_import == transaction) {
		active_import = NULL;
	}
	k_mutex_unlock(&content_lock);
	return ret;
}

int papermono_content_store_delete(const struct papermono_content_entry *entry)
{
	const char *directory;
	int ret;

	if (entry == NULL) {
		return -EINVAL;
	}
	directory = kind_directory(entry->kind);
	if (!content_entry_path_valid(entry, directory)) {
		return -EPERM;
	}
	ret = papermono_content_store_poll();
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&content_lock, K_FOREVER);
	ret = fs_unlink(entry->path);
	k_mutex_unlock(&content_lock);
	return ret;
}
