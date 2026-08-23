#ifndef MICROPIXEL_RUNTIME_ABI_ABI_BRIDGE_H
#define MICROPIXEL_RUNTIME_ABI_ABI_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

bool micropixel_register_native_apis(void);
int32_t micropixel_runtime_event_wait(wasm_exec_env_t exec_env, micropixel_event_t* event_out, uint32_t event_capacity,
                                      uint64_t timeout_us);
micropixel_app_time_t micropixel_runtime_clock_now(wasm_exec_env_t exec_env);
int32_t micropixel_runtime_service_open(wasm_exec_env_t exec_env, uint32_t service_id,
                                        uint32_t required_interface_version, micropixel_service_info_t* info_out,
                                        uint32_t info_capacity);
int32_t micropixel_runtime_service_call(wasm_exec_env_t exec_env, micropixel_service_handle_t service,
                                        uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                        uint8_t* response, uint32_t response_capacity, uint32_t* response_size_out);
int32_t micropixel_runtime_service_submit(wasm_exec_env_t exec_env, micropixel_service_handle_t service,
                                          uint32_t channel_id, const uint8_t* bytes, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif
