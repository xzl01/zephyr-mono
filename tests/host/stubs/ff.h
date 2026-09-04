#ifndef HOST_FF_H_
#define HOST_FF_H_

#include <stdint.h>

#define FR_OK 0
#define AM_DIR 0x10U

typedef int FRESULT;
typedef struct { int unused; } FATFS;
typedef struct { int unused; } DIR;
typedef struct {
	uint32_t fsize;
	uint16_t fdate;
	uint16_t ftime;
	uint8_t fattrib;
	char fname[256];
} FILINFO;

FRESULT f_opendir(DIR *directory, const char *path);
FRESULT f_readdir(DIR *directory, FILINFO *info);
FRESULT f_closedir(DIR *directory);

#endif
