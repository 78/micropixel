// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/knob_input.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::sensecap_watcher {
namespace {

// A full Grey-code quadrature table. The index is (previous << 2) | current,
// where each two-bit state is (phase A << 1) | phase B. Only the eight valid
// one-step transitions produce a delta; a skipped or repeated state yields 0,
// which is what rejects contact bounce without any debounce timer.
DRAM_ATTR const int8_t kQuadratureDelta[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0,
};

// A human cannot turn the knob faster than this between two worker wake-ups, so
// anything above it is electrical noise and is dropped rather than flooded into
// the Input queue.
constexpr int32_t kMaxStepsPerWake = 8;

constexpr uint32_t kTaskStackBytes = 3072U;
constexpr UBaseType_t kTaskPriority = 4U;

}  // namespace

KnobInput::~KnobInput() { Stop(); }

esp_err_t KnobInput::Initialize(device::Input& input) {
    if (input_ != nullptr || worker_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    input_ = &input;

    gpio_config_t pin_config{};
    pin_config.pin_bit_mask =
        (1ULL << static_cast<uint32_t>(config_.phase_a)) | (1ULL << static_cast<uint32_t>(config_.phase_b));
    pin_config.mode = GPIO_MODE_INPUT;
    pin_config.pull_up_en = GPIO_PULLUP_ENABLE;
    pin_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pin_config.intr_type = GPIO_INTR_ANYEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&pin_config), config_.log_tag, "configure knob phases failed");

    previous_phase_ = static_cast<uint8_t>((gpio_get_level(config_.phase_a) << 1) | gpio_get_level(config_.phase_b));

    // Another input may already own the shared GPIO ISR service, so an
    // already-installed service is not an error.
    const esp_err_t service = gpio_install_isr_service(0);
    if (service != ESP_OK && service != ESP_ERR_INVALID_STATE) {
        return service;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(config_.phase_a, OnEdge, this), config_.log_tag,
                        "attach knob phase A failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(config_.phase_b, OnEdge, this), config_.log_tag,
                        "attach knob phase B failed");

    if (xTaskCreatePinnedToCore(WorkerEntry, config_.task_name, kTaskStackBytes, this, kTaskPriority, &worker_,
                                config_.task_core) != pdPASS) {
        worker_ = nullptr;
        (void)gpio_isr_handler_remove(config_.phase_a);
        (void)gpio_isr_handler_remove(config_.phase_b);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(config_.log_tag, "knob ready: GPIO%d/GPIO%d quadrature, switch polled every %u ms",
             static_cast<int>(config_.phase_a), static_cast<int>(config_.phase_b),
             static_cast<unsigned>(config_.poll_interval_ms));
    return ESP_OK;
}

void IRAM_ATTR KnobInput::OnEdge(void* context) {
    auto* knob = static_cast<KnobInput*>(context);
    if (knob == nullptr) {
        return;
    }
    const uint32_t current =
        static_cast<uint32_t>((gpio_get_level(knob->config_.phase_a) << 1) | gpio_get_level(knob->config_.phase_b));
    const uint32_t index = (static_cast<uint32_t>(knob->previous_phase_) << 2U) | current;
    knob->previous_phase_ = static_cast<uint8_t>(current);
    const int8_t delta = kQuadratureDelta[index & 0x0FU];
    if (delta == 0) {
        return;
    }
    knob->rotation_steps_.fetch_add(delta, std::memory_order_relaxed);
    BaseType_t higher_priority_woken = pdFALSE;
    if (knob->worker_ != nullptr) {
        vTaskNotifyGiveFromISR(knob->worker_, &higher_priority_woken);
    }
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void KnobInput::WorkerEntry(void* context) { static_cast<KnobInput*>(context)->Worker(); }

void KnobInput::Worker() {
    const TickType_t poll_interval = pdMS_TO_TICKS(config_.poll_interval_ms);
    while (!stopping_.load(std::memory_order_acquire)) {
        // Wake on a rotation edge, or on the poll cadence so the switch is still
        // sampled while the knob is still.
        (void)ulTaskNotifyTake(pdTRUE, poll_interval);
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }
        const int32_t steps = rotation_steps_.exchange(0, std::memory_order_relaxed);
        if (steps != 0) {
            EmitRotation(steps);
        }
        if (config_.read_pressed != nullptr) {
            UpdateButton(config_.read_pressed(config_.read_pressed_context),
                         static_cast<uint64_t>(esp_timer_get_time()));
        }
    }
}

void KnobInput::EmitRotation(int32_t steps) {
    if (input_ == nullptr) {
        return;
    }
    // The decoder counted raw quadrature edges; the Host and the Guests both
    // want gestures, so fold them into whole detents and carry the leftover.
    const int32_t per_detent = config_.steps_per_detent == 0U ? 1 : static_cast<int32_t>(config_.steps_per_detent);
    rotation_remainder_ += steps;
    const int32_t detents = rotation_remainder_ / per_detent;
    rotation_remainder_ -= detents * per_detent;
    if (detents == 0) {
        return;
    }
    steps = detents;
    if (steps > kMaxStepsPerWake) {
        steps = kMaxStepsPerWake;
    }
    if (steps < -kMaxStepsPerWake) {
        steps = -kMaxStepsPerWake;
    }
    const device::KeyCode code = steps > 0 ? config_.clockwise_code : config_.anticlockwise_code;
    const uint32_t count = static_cast<uint32_t>(steps > 0 ? steps : -steps);
    NotifyNavigation(steps, false, false);
    for (uint32_t index = 0; index < count; ++index) {
        const uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        (void)input_->InjectKey(
            {.timestamp_us = timestamp_us, .code = code, .phase = device::KeyPhase::kDown, .repeat_count = index});
        (void)input_->InjectKey(
            {.timestamp_us = timestamp_us, .code = code, .phase = device::KeyPhase::kUp, .repeat_count = index});
    }
}

void KnobInput::EmitClick(device::KeyCode code) {
    if (input_ == nullptr) {
        return;
    }
    // A resolved gesture is a complete click, so both edges are injected
    // together with one timestamp: consumers act on the edge, and a
    // half-delivered click would look to them like a stuck key.
    const uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    (void)input_->InjectKey(
        {.timestamp_us = timestamp_us, .code = code, .phase = device::KeyPhase::kDown, .repeat_count = 0U});
    (void)input_->InjectKey(
        {.timestamp_us = timestamp_us, .code = code, .phase = device::KeyPhase::kUp, .repeat_count = 0U});
}

void KnobInput::UpdateButton(bool pressed, uint64_t now_us) {
    if (pressed != button_pressed_) {
        button_pressed_ = pressed;
        if (pressed) {
            press_started_us_ = now_us;
            long_press_fired_ = false;
            return;
        }
        // Released before the hold window, so this press is a click. A second
        // click inside the window turns the pair into the back gesture, which
        // cancels the primary action the first click is still waiting to be.
        if (!long_press_fired_) {
            if (clicks_in_window_ == 0U) {
                clicks_in_window_ = 1U;
                click_released_us_ = now_us;
            } else {
                clicks_in_window_ = 0U;
                EmitClick(config_.double_click_code);
                NotifyNavigation(0, false, true);
            }
        }
        long_press_fired_ = false;
        return;
    }

    if (button_pressed_) {
        const uint64_t hold_us = static_cast<uint64_t>(config_.long_press_ms) * 1000U;
        if (!long_press_fired_ && (now_us - press_started_us_) >= hold_us) {
            // A hold is a power request, not a click, so any click waiting for a
            // partner is dropped: the Host acts on the hold, and a click
            // reported beside it would look like the user selecting something on
            // the way down.
            long_press_fired_ = true;
            clicks_in_window_ = 0U;
            RequestPowerOff(now_us);
        }
        return;
    }

    const uint64_t window_us = static_cast<uint64_t>(config_.double_click_window_ms) * 1000U;
    if (clicks_in_window_ != 0U && (now_us - click_released_us_) > window_us) {
        // No partner arrived, so this was the primary action: report it to
        // Guests and let the Host activate whatever holds the focus.
        clicks_in_window_ = 0U;
        EmitClick(config_.single_click_code);
        NotifyNavigation(0, true, false);
    }
}

void KnobInput::RequestPowerOff(uint64_t timestamp_us) {
    LongPressSink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    if (long_press_sink_ != nullptr) {
        sink = long_press_sink_;
        context = long_press_context_;
        ++sink_inflight_;
    }
    portEXIT_CRITICAL(&sink_lock_);
    if (sink == nullptr) {
        ESP_LOGD(config_.log_tag, "knob held with no power request sink bound");
        return;
    }
    const bool accepted = sink(context, timestamp_us);
    portENTER_CRITICAL(&sink_lock_);
    --sink_inflight_;
    portEXIT_CRITICAL(&sink_lock_);
    ESP_LOGI(config_.log_tag, "knob hold: Host %s the power request", accepted ? "accepted" : "declined");
}

void KnobInput::NotifyNavigation(int32_t rotation_steps, bool confirm, bool back) {
    NavigationSink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    sink = navigation_sink_;
    context = navigation_context_;
    portEXIT_CRITICAL(&sink_lock_);
    if (sink != nullptr) {
        sink(context, rotation_steps, confirm, back);
    }
}

void KnobInput::SetNavigationSink(NavigationSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    navigation_sink_ = sink;
    navigation_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void KnobInput::SetLongPressSink(LongPressSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    long_press_sink_ = sink;
    long_press_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
    if (sink != nullptr) {
        return;
    }
    // The caller is unbinding to go away, so wait for a call that is already
    // running before it can be destroyed under us.
    for (;;) {
        portENTER_CRITICAL(&sink_lock_);
        const uint32_t inflight = sink_inflight_;
        portEXIT_CRITICAL(&sink_lock_);
        if (inflight == 0U) {
            break;
        }
        vTaskDelay(1U);
    }
}

void KnobInput::Stop() {
    if (worker_ == nullptr) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    (void)gpio_isr_handler_remove(config_.phase_a);
    (void)gpio_isr_handler_remove(config_.phase_b);
    (void)xTaskNotifyGive(worker_);
    // The knob belongs to the process-lifetime Board, so this only runs if the
    // board is ever torn down; deleting the worker is the whole cleanup needed.
    vTaskDelete(worker_);
    worker_ = nullptr;
}

}  // namespace micropixel::platform::sensecap_watcher
