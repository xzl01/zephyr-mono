/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/storage/disk_access.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_STORAGE_DISK_ACCESS_H_
#define PAPERMONO_PREVIEW_ZEPHYR_STORAGE_DISK_ACCESS_H_

#define DISK_IOCTL_CTRL_INIT 1
#define DISK_IOCTL_CTRL_DEINIT 2
#define DISK_IOCTL_GET_SECTOR_COUNT 3
#define DISK_STATUS_UNINIT 0x01
#define DISK_STATUS_NOMEDIA 0x02

int disk_access_ioctl(const char *name, int command, void *buffer);
int disk_access_status(const char *name);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_STORAGE_DISK_ACCESS_H_ */
