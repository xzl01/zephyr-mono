/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/fs/fs.h -- backed by a real host directory. */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_FS_FS_H_
#define PAPERMONO_PREVIEW_ZEPHYR_FS_FS_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_FILE_NAME 255
#define FS_O_READ 0x01
#define FS_O_WRITE 0x02
#define FS_O_CREATE 0x10
#define FS_O_APPEND 0x20
#define FS_O_TRUNC 0x40
#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2
#define FS_DIR_ENTRY_FILE 1
#define FS_DIR_ENTRY_DIR 2
#define FS_MOUNT_FLAG_NO_FORMAT 0x01
#define FS_MOUNT_FLAG_USE_DISK_ACCESS 0x02
#define FS_FATFS 1

struct fs_file_t {
	int fd;
	int flags;
};

struct fs_dirent {
	char name[MAX_FILE_NAME + 1];
	size_t size;
	int type;
};

struct fs_dir_t {
	void *dir;
};

struct fs_statvfs {
	size_t f_bsize;
	size_t f_frsize;
	size_t f_blocks;
	size_t f_bfree;
	size_t f_bavail;
	size_t f_ffree;
	size_t f_favail;
};

struct fs_mount_t {
	int type;
	const char *mnt_point;
	void *fs_data;
	void *storage_dev;
	int flags;
};

static inline void fs_file_t_init(struct fs_file_t *file)
{
	file->fd = -1;
	file->flags = 0;
}

static inline void fs_dir_t_init(struct fs_dir_t *directory)
{
	directory->dir = NULL;
}

int fs_open(struct fs_file_t *file, const char *path, int flags);
ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length);
ssize_t fs_write(struct fs_file_t *file, const void *buffer, size_t length);
int fs_seek(struct fs_file_t *file, off_t offset, int whence);
off_t fs_tell(struct fs_file_t *file);
int fs_truncate(struct fs_file_t *file, off_t length);
int fs_sync(struct fs_file_t *file);
int fs_close(struct fs_file_t *file);
int fs_unlink(const char *path);
int fs_rename(const char *from, const char *to);
int fs_mkdir(const char *path);
int fs_opendir(struct fs_dir_t *directory, const char *path);
int fs_readdir(struct fs_dir_t *directory, struct fs_dirent *entry);
int fs_closedir(struct fs_dir_t *directory);
int fs_stat(const char *path, struct fs_dirent *entry);
int fs_statvfs(const char *path, struct fs_statvfs *stat);
int fs_mkfs(int fs_type, uintptr_t dev_id, void *cfg, int flags);
int fs_mount(struct fs_mount_t *mount);
int fs_unmount(struct fs_mount_t *mount);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_FS_FS_H_ */
