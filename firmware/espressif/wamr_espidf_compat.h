#pragma once

/* Declarations WAMR 2.4.x used to receive through transitive includes. */
#include <stdio.h>
#include <sys/stat.h>

/*
 * WAMR's ESP-IDF platform adapter defines renameat() in espidf_platform.c,
 * but ESP-IDF 6.1's libc headers no longer declare it for espidf_file.c.
 */
#ifdef ESP_PLATFORM
int renameat(int old_dirfd, const char *old_path,
             int new_dirfd, const char *new_path);
#endif
