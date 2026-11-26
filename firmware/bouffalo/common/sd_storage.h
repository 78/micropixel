// SPDX-License-Identifier: Apache-2.0
#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdbool.h>

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_STORAGE_MOUNT_PATH "/sd"

bool sd_storage_mount(void);
bool sd_storage_reconnect(void);
void sd_storage_release(void);
bool sd_storage_is_mounted(void);
FRESULT sd_storage_last_result(void);

#ifdef __cplusplus
}
#endif

#endif
