// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void mpx_console_frame(const char*, const void*, size_t, const char*);
void mpx_console_line(const char*);
void mpx_console_init(void);
int mpx_console_read(void);
#ifdef __cplusplus
}
#endif
