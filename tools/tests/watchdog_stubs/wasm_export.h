#ifndef MICROPIXEL_TEST_WATCHDOG_STUB_WASM_EXPORT_H
#define MICROPIXEL_TEST_WATCHDOG_STUB_WASM_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct test_wasm_module* wasm_module_inst_t;
typedef struct test_wasm_exec_env* wasm_exec_env_t;
typedef struct test_wasm_function* wasm_function_inst_t;

bool wasm_runtime_call_wasm(wasm_exec_env_t exec_env, wasm_function_inst_t function, uint32_t argc, uint32_t argv[]);
void wasm_runtime_terminate(wasm_module_inst_t module_inst);

#endif
