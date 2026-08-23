#ifndef MICROPIXEL_RUNTIME_WAMR_DIAGNOSTICS_H
#define MICROPIXEL_RUNTIME_WAMR_DIAGNOSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

void micropixel_log_heap_state(const char* label);
void micropixel_log_stack_profiles(void);

#ifdef __cplusplus
}
#endif

#endif
