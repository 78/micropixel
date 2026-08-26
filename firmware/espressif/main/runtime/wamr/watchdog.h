#ifndef MICROPIXEL_RUNTIME_WAMR_WATCHDOG_H
#define MICROPIXEL_RUNTIME_WAMR_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

void micropixel_watchdog_checkpoint(void);
void micropixel_watchdog_pause(void);
void micropixel_watchdog_resume(void);
bool micropixel_call_with_watchdog(wasm_module_inst_t module_inst, wasm_exec_env_t exec_env,
                                   wasm_function_inst_t function, uint32_t argc, uint32_t argv[], uint32_t timeout_ms,
                                   bool* timed_out_out);

#ifdef __cplusplus
}
#endif

#endif
