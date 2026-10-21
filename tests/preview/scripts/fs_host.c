/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host filesystem backend rooted at $PAPERMONO_PREVIEW_FS.  Kept in its own
 * translation unit because <dirent.h> and the FatFs shim both want to own the
 * name "DIR".
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zephyr/fs/fs.h>

/* ---------------------------------------------------------------- fs (real) */

static const char *fs_root(void)
{
	const char *root = getenv("PAPERMONO_PREVIEW_FS");

	return (root != NULL && root[0] != '\0') ? root :
		"/tmp/papermono-preview/build/fs";
}

/*
 * The Zephyr VFS speaks "/SD:/papermono/books/x.txt" while FatFs (which
 * papermono_content_store.c uses directly for directory scans) speaks
 * "SD:/papermono/books".  Both, plus plain "/papermono/..." and a relative
 * "papermono/...", map into the same host root.
 */
static void fs_resolve(const char *path, char *out, size_t out_size)
{
	const char *suffix = path;
	char normalised[1024];

	if (strncmp(suffix, "/SD:", 4) == 0) {
		suffix += 4;
	} else if (strncmp(suffix, "SD:", 3) == 0) {
		suffix += 3;
	} else if (strncmp(suffix, "0:", 2) == 0) {
		suffix += 2;
	}
	if (suffix[0] == '\0') {
		snprintf(normalised, sizeof(normalised), "/");
	} else if (suffix[0] == '/') {
		snprintf(normalised, sizeof(normalised), "%s", suffix);
	} else {
		snprintf(normalised, sizeof(normalised), "/%s", suffix);
	}
	snprintf(out, out_size, "%s%s", fs_root(), normalised);
}

struct host_dir {
	DIR *dir;
	char path[1024];
};

int fs_open(struct fs_file_t *file, const char *path, int flags)
{
	char resolved[1024];
	int oflags = 0;

	if (file == NULL || path == NULL) {
		return -EINVAL;
	}
	fs_resolve(path, resolved, sizeof(resolved));
	if ((flags & FS_O_READ) != 0 && (flags & FS_O_WRITE) != 0) {
		oflags = O_RDWR;
	} else if ((flags & FS_O_WRITE) != 0) {
		oflags = O_WRONLY;
	} else {
		oflags = O_RDONLY;
	}
	if ((flags & FS_O_CREATE) != 0) {
		oflags |= O_CREAT;
	}
	if ((flags & FS_O_TRUNC) != 0) {
		oflags |= O_TRUNC;
	}
	file->fd = open(resolved, oflags, 0644);
	file->flags = flags;
	return file->fd < 0 ? -errno : 0;
}

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	ssize_t ret;

	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	ret = read(file->fd, buffer, length);
	return ret < 0 ? -errno : ret;
}

ssize_t fs_write(struct fs_file_t *file, const void *buffer, size_t length)
{
	ssize_t ret;

	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	ret = write(file->fd, buffer, length);
	return ret < 0 ? -errno : ret;
}

int fs_seek(struct fs_file_t *file, off_t offset, int whence)
{
	int host_whence = SEEK_SET;

	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	if (whence == FS_SEEK_CUR) {
		host_whence = SEEK_CUR;
	} else if (whence == FS_SEEK_END) {
		host_whence = SEEK_END;
	}
	return lseek(file->fd, offset, host_whence) < 0 ? -errno : 0;
}

off_t fs_tell(struct fs_file_t *file)
{
	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	return lseek(file->fd, 0, SEEK_CUR);
}

int fs_truncate(struct fs_file_t *file, off_t length)
{
	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	return ftruncate(file->fd, length) < 0 ? -errno : 0;
}

int fs_sync(struct fs_file_t *file)
{
	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	return fsync(file->fd) < 0 ? -errno : 0;
}

int fs_close(struct fs_file_t *file)
{
	if (file == NULL || file->fd < 0) {
		return -EBADF;
	}
	(void)close(file->fd);
	file->fd = -1;
	return 0;
}

int fs_unlink(const char *path)
{
	char resolved[1024];

	fs_resolve(path, resolved, sizeof(resolved));
	return unlink(resolved) < 0 ? -errno : 0;
}

int fs_rename(const char *from, const char *to)
{
	char resolved_from[1024];
	char resolved_to[1024];

	fs_resolve(from, resolved_from, sizeof(resolved_from));
	fs_resolve(to, resolved_to, sizeof(resolved_to));
	return rename(resolved_from, resolved_to) < 0 ? -errno : 0;
}

int fs_mkdir(const char *path)
{
	char resolved[1024];

	fs_resolve(path, resolved, sizeof(resolved));
	return mkdir(resolved, 0755) < 0 && errno != EEXIST ? -errno : 0;
}

int fs_opendir(struct fs_dir_t *directory, const char *path)
{
	struct host_dir *host;
	char resolved[1024];

	if (directory == NULL || path == NULL) {
		return -EINVAL;
	}
	fs_resolve(path, resolved, sizeof(resolved));
	host = calloc(1, sizeof(*host));
	if (host == NULL) {
		return -ENOMEM;
	}
	host->dir = opendir(resolved);
	if (host->dir == NULL) {
		const int err = errno;

		free(host);
		return -err;
	}
	snprintf(host->path, sizeof(host->path), "%s", resolved);
	directory->dir = host;
	return 0;
}

int fs_readdir(struct fs_dir_t *directory, struct fs_dirent *entry)
{
	struct host_dir *host;
	struct dirent *host_entry;
	struct stat st;
	char full[2048];

	if (directory == NULL || directory->dir == NULL || entry == NULL) {
		return -EINVAL;
	}
	host = directory->dir;
	memset(entry, 0, sizeof(*entry));
	for (;;) {
		host_entry = readdir(host->dir);
		if (host_entry == NULL) {
			return 0;
		}
		if (strcmp(host_entry->d_name, ".") == 0 ||
		    strcmp(host_entry->d_name, "..") == 0) {
			continue;
		}
		break;
	}
	snprintf(entry->name, sizeof(entry->name), "%s", host_entry->d_name);
	snprintf(full, sizeof(full), "%s/%s", host->path, host_entry->d_name);
	if (stat(full, &st) == 0) {
		entry->size = (size_t)st.st_size;
		entry->type = S_ISDIR(st.st_mode) ? FS_DIR_ENTRY_DIR :
			FS_DIR_ENTRY_FILE;
	}
	return 0;
}

int fs_closedir(struct fs_dir_t *directory)
{
	struct host_dir *host;

	if (directory == NULL || directory->dir == NULL) {
		return -EINVAL;
	}
	host = directory->dir;
	(void)closedir(host->dir);
	free(host);
	directory->dir = NULL;
	return 0;
}

int fs_stat(const char *path, struct fs_dirent *entry)
{
	char resolved[1024];
	struct stat st;

	if (path == NULL || entry == NULL) {
		return -EINVAL;
	}
	fs_resolve(path, resolved, sizeof(resolved));
	memset(entry, 0, sizeof(*entry));
	if (stat(resolved, &st) != 0) {
		return -errno;
	}
	entry->size = (size_t)st.st_size;
	entry->type = S_ISDIR(st.st_mode) ? FS_DIR_ENTRY_DIR : FS_DIR_ENTRY_FILE;
	{
		const char *base = strrchr(resolved, '/');

		snprintf(entry->name, sizeof(entry->name), "%s",
			base != NULL ? base + 1 : resolved);
	}
	return 0;
}

int fs_statvfs(const char *path, struct fs_statvfs *stat)
{
	(void)path;
	if (stat == NULL) {
		return -EINVAL;
	}
	memset(stat, 0, sizeof(*stat));
	stat->f_bsize = 4096U;
	stat->f_frsize = 4096U;
	stat->f_blocks = 1024U * 1024U;
	stat->f_bfree = 512U * 1024U;
	stat->f_bavail = stat->f_bfree;
	return 0;
}

/* Preview never formats or recursively erases anything on the host. */
int fs_mkfs(int fs_type, uintptr_t dev_id, void *cfg, int flags)
{
	(void)fs_type;
	(void)dev_id;
	(void)cfg;
	(void)flags;
	return -ENOTSUP;
}

static int preview_mount_result;

/* Preview-only injection, used to reach the real storage model's unformatted
 * state. It never formats or mounts a host disk. */
void preview_fs_mount_result(int result)
{
	preview_mount_result = result;
}

int fs_mount(struct fs_mount_t *mount)
{
	(void)mount;
	return preview_mount_result;
}

int fs_unmount(struct fs_mount_t *mount)
{
	(void)mount;
	return 0;
}
