#ifndef MICROPIXEL_RUNTIME_WAMR_DIAGNOSTICS_H
#define MICROPIXEL_RUNTIME_WAMR_DIAGNOSTICS_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_MICROPIXEL_HEAP_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_CHECKPOINTS
void micropixel_check_heap(const char* stage);
#else
static inline void micropixel_check_heap(const char* stage) { (void)stage; }
#endif

void micropixel_log_heap_state(const char* label);
void micropixel_log_stack_profiles(void);

#ifdef __cplusplus
}
#endif

#endif
