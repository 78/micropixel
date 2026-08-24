#include <cinttypes>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "esp_log.h"
#include "runtime/abi/abi_bridge.h"
#include "runtime/guest_context.hpp"
#include "runtime/wamr/watchdog.h"
#include "wasm_export.h"

namespace {

constexpr char kTag[] = "micropixel_abi";

micropixel::runtime::GuestContext* GetContext(wasm_exec_env_t exec_env) {
    if (exec_env == nullptr) {
        return nullptr;
    }
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
    return static_cast<micropixel::runtime::GuestContext*>(wasm_runtime_get_custom_data(instance));
}

}  // namespace

extern "C" int32_t micropixel_runtime_event_wait(wasm_exec_env_t exec_env, micropixel_event_t* event_out,
                                                 uint32_t event_capacity, uint64_t timeout_us) {
    micropixel_watchdog_checkpoint();
    if (event_out == nullptr || event_capacity < sizeof(micropixel_event_t)) {
        ESP_LOGW(kTag, "event_wait rejected buffer: pointer=%p capacity=%" PRIu32 " required=%zu", event_out,
                 event_capacity, sizeof(micropixel_event_t));
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }

    auto* context = GetContext(exec_env);
    if (context == nullptr || !context->valid()) {
        return MICROPIXEL_STATUS_INTERNAL;
    }

    micropixel_event_t event{};
    micropixel_watchdog_pause();
    micropixel::runtime::EventWaitResult result = context->WaitEvent(event, timeout_us);
    micropixel_watchdog_resume();
    if (result == micropixel::runtime::EventWaitResult::kTimeout) {
        return MICROPIXEL_STATUS_TIMEOUT;
    }
    if (result == micropixel::runtime::EventWaitResult::kClosed) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    *event_out = event;
    context->NoteEventDelivered(event);
    return MICROPIXEL_STATUS_OK;
}

extern "C" micropixel_app_time_t micropixel_runtime_clock_now(wasm_exec_env_t exec_env) {
    micropixel_watchdog_checkpoint();
    auto* context = GetContext(exec_env);
    return context != nullptr ? context->AppTimeNow() : 0U;
}

extern "C" int32_t micropixel_runtime_service_open(wasm_exec_env_t exec_env, uint32_t service_id,
                                                   uint32_t required_interface_version,
                                                   micropixel_service_info_t* info_out, uint32_t info_capacity) {
    micropixel_watchdog_checkpoint();
    if (info_out == nullptr || info_capacity < sizeof(*info_out)) {
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }
    auto* context = GetContext(exec_env);
    return context != nullptr ? context->ServiceOpen(service_id, required_interface_version, *info_out, info_capacity)
                              : static_cast<int32_t>(MICROPIXEL_STATUS_INTERNAL);
}

extern "C" int32_t micropixel_runtime_service_call(wasm_exec_env_t exec_env, micropixel_service_handle_t service,
                                                   uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                                   uint8_t* response, uint32_t response_capacity,
                                                   uint32_t* response_size_out) {
    micropixel_watchdog_checkpoint();
    if (response_size_out == nullptr || (request == nullptr && request_size != 0U) ||
        (response == nullptr && response_capacity != 0U)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    auto* context = GetContext(exec_env);
    if (context == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    micropixel_watchdog_pause();
    const int32_t status = context->ServiceCall(service, method_id, request, request_size, response, response_capacity,
                                                *response_size_out);
    micropixel_watchdog_resume();
    return status;
}

extern "C" int32_t micropixel_runtime_service_submit(wasm_exec_env_t exec_env, micropixel_service_handle_t service,
                                                     uint32_t channel_id, const uint8_t* bytes, uint32_t length) {
    micropixel_watchdog_checkpoint();
    if (bytes == nullptr || length == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    auto* context = GetContext(exec_env);
    return context != nullptr ? context->ServiceSubmit(service, channel_id, bytes, length)
                              : static_cast<int32_t>(MICROPIXEL_STATUS_INTERNAL);
}
