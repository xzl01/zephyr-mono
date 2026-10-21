/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: ff.h (FatFs) */
#ifndef PAPERMONO_PREVIEW_FF_H_
#define PAPERMONO_PREVIEW_FF_H_

#include <stdint.h>

#define FR_OK 0
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_INVALID_NAME 6
#define FR_DENIED 7
#define FR_EXIST 8
#define FR_INVALID_OBJECT 9
#define FR_WRITE_PROTECTED 10
#define FR_INVALID_DRIVE 11
#define FR_NOT_ENABLED 12
#define FR_NO_FILESYSTEM 13
#define AM_DIR 0x10U
#define AM_HID 0x02U
#define AM_SYS 0x04U
#define AM_RDO 0x01U
#define AM_ARC 0x20U
#define FA_READ 0x01U
#define FA_WRITE 0x02U
#define FA_OPEN_EXISTING 0x00U
#define FA_CREATE_NEW 0x04U
#define FA_CREATE_ALWAYS 0x08U

typedef int FRESULT;
typedef struct {
	int unused;
} FATFS;
typedef struct {
	void *dir;
} DIR;
typedef struct {
	uint32_t fsize;
	uint16_t fdate;
	uint16_t ftime;
	uint8_t fattrib;
	char fname[256];
} FILINFO;
typedef struct {
	void *file;
} FIL;

FRESULT f_opendir(DIR *directory, const char *path);
FRESULT f_readdir(DIR *directory, FILINFO *info);
FRESULT f_closedir(DIR *directory);
FRESULT f_stat(const char *path, FILINFO *info);
FRESULT f_unlink(const char *path);

#endif /* PAPERMONO_PREVIEW_FF_H_ */
