// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef uint64_t FSIZE_t;
typedef enum { FR_OK, FR_DISK_ERR, FR_NO_FILE, FR_NO_PATH, FR_EXIST, FR_DENIED } FRESULT;
typedef struct {
    FSIZE_t pos;
    void* node;
    DWORD* cltbl;
} FIL;
typedef struct {
    FSIZE_t fsize;
    uint8_t fattrib;
} FILINFO;
#define FF_USE_EXPAND 1
#define FF_USE_FASTSEEK 1
#define CREATE_LINKMAP UINT64_MAX
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_NEW 4
#define AM_DIR 16
#define f_tell(fp) ((fp)->pos)
FSIZE_t mock_size(FIL*);
#define f_size(fp) mock_size(fp)
FRESULT f_stat(const char*, FILINFO*);
FRESULT f_open(FIL*, const char*, uint8_t);
FRESULT f_mkdir(const char*);
FRESULT f_expand(FIL*, FSIZE_t, uint8_t);
FRESULT f_lseek(FIL*, FSIZE_t);
FRESULT f_sync(FIL*);
FRESULT f_close(FIL*);
FRESULT f_rename(const char*, const char*);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
