#include "conformance/guest_test_hooks.hpp"

#include <cinttypes>
#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::conformance {

#if CONFIG_MICROPIXEL_ENABLE_CONFORMANCE_TEST_HOOKS

namespace {

constexpr char kTag[] = "micropixel_test";
constexpr char kEventWaitMarker[] = "__micropixel_test_event_wait";
constexpr char kTouchPressureMarker[] = "__micropixel_test_touch_pressure";
constexpr char kRunHandlerMarker[] = "__micropixel_test_run_handler";

}  // namespace

GuestTestHooks::GuestTestHooks(wasm_module_inst_t instance, wasm_exec_env_t, runtime::GuestContext& context) {
    const bool event_wait = wasm_runtime_lookup_function(instance, kEventWaitMarker) != nullptr;
    const bool touch_pressure = wasm_runtime_lookup_function(instance, kTouchPressureMarker) != nullptr;
    const bool run_handler = wasm_runtime_lookup_function(instance, kRunHandlerMarker) != nullptr;
    if (!event_wait && !touch_pressure && !run_handler) {
        return;
    }

    context_ = &context;
    kind_ = touch_pressure ? Kind::kTouchPressure : (run_handler ? Kind::kRunHandlerStop : Kind::kHostWake);
    if (pthread_create(&thread_, nullptr, InjectEventBurst, this) != 0) {
        ESP_LOGE(kTag, "failed to start conformance event injector");
        ready_ = false;
        return;
    }
    started_ = true;
}

GuestTestHooks::~GuestTestHooks() {
    if (started_) {
        (void)pthread_join(thread_, nullptr);
    }
}

void* GuestTestHooks::InjectEventBurst(void* argument) {
    auto& hooks = *static_cast<GuestTestHooks*>(argument);
    if (hooks.kind_ == Kind::kRunHandlerStop) {
        vTaskDelay(pdMS_TO_TICKS(150));
        micropixel_event_t event{};
        event.size = sizeof(event);
        event.event_id = MICROPIXEL_CORE_EVENT_STOP;
        event.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        event.sequence = 1U;
        if (!hooks.context_->PushRequired(event)) {
            ESP_LOGE(kTag, "failed to inject Run handler Stop");
            return nullptr;
        }
        ESP_LOGI(kTag, "injected Run handler Stop");
        return nullptr;
    }
    if (hooks.kind_ == Kind::kTouchPressure) {
        constexpr uint32_t kMoveCount = 5000U;
        vTaskDelay(pdMS_TO_TICKS(150));

        device::TouchSample sample{};
        sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        sample.phase = device::TouchPhase::kDown;
        sample.id = 7U;
        sample.x = 20U;
        sample.y = 30U;
        sample.pressure_per_mille = 0U;
        if (!hooks.context_->PushTouch(sample)) {
            ESP_LOGE(kTag, "failed to inject required Touch Down");
            return nullptr;
        }

        for (uint32_t index = 0U; index < kMoveCount; ++index) {
            sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            sample.phase = device::TouchPhase::kMove;
            sample.x = static_cast<int32_t>(40U + (index % 640U));
            sample.y = static_cast<int32_t>(50U + ((index * 3U) % 620U));
            if (!hooks.context_->PushTouch(sample)) {
                ESP_LOGE(kTag, "failed to inject Touch Move #" PRIu32, index + 1U);
                return nullptr;
            }
        }

        sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        sample.phase = device::TouchPhase::kUp;
        sample.pressure_per_mille = 0U;
        if (!hooks.context_->PushTouch(sample)) {
            ESP_LOGE(kTag, "failed to inject required Touch Up");
            return nullptr;
        }
        ESP_LOGI(kTag, "injected Touch pressure: Down + " PRIu32 " Move + Up", kMoveCount);
        return nullptr;
    }

    constexpr uint32_t kDelaysMs[] = {150U, 75U, 75U};
    for (uint32_t index = 0; index < 3U; ++index) {
        vTaskDelay(pdMS_TO_TICKS(kDelaysMs[index]));
        micropixel_event_t event{};
        event.size = sizeof(event);
        event.event_id = MICROPIXEL_CORE_EVENT_HOST_WAKE;
        event.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        event.sequence = index + 1U;
        if (!hooks.context_->PushRequired(event)) {
            ESP_LOGE(kTag, "failed to inject required event " PRIu32, event.sequence);
            return nullptr;
        }
        ESP_LOGI(kTag, "injected HostWake sequence=" PRIu32, event.sequence);
    }
    return nullptr;
}

#endif

}  // namespace micropixel::conformance
