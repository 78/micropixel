// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
typedef TaskHandle_t korp_tid;
typedef TaskHandle_t korp_thread;
typedef SemaphoreHandle_t korp_mutex;
typedef SemaphoreHandle_t korp_rwlock;
typedef SemaphoreHandle_t korp_sem;
typedef struct {
    SemaphoreHandle_t semaphore;
    unsigned waiters;
} korp_cond;
typedef int os_file_handle;
typedef int os_raw_file_handle;
typedef void *os_dir_stream;
typedef int os_poll_file_handle;
typedef unsigned os_nfds_t;
typedef int os_timespec;
#define OS_THREAD_MUTEX_INITIALIZER NULL
#define BH_APPLET_PRESERVED_STACK_SIZE (2 * BH_KB)
#define BH_THREAD_DEFAULT_PRIORITY 3
static inline int os_getpagesize(void) { return 4096; }
static inline os_file_handle os_get_invalid_handle(void) { return -1; }
