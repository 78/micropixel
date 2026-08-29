#include "platform/input/esp_lcd_touch_input.hpp"

#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace micropixel::platform::input {
namespace {

constexpr char kTag[] = "micropixel_touch";

}  // namespace

EspLcdTouchInput* EspLcdTouchInput::active_instance_ = nullptr;

EspLcdTouchInput::EspLcdTouchInput(int32_t width, int32_t height) : width_(width), height_(height) {}

esp_err_t EspLcdTouchInput::Initialize(esp_lcd_touch_handle_t touch, buses::I2cExecutor& executor) {
    if (touch == nullptr || width_ <= 0 || height_ <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    touch_ = touch;
    executor_ = &executor;
    return ESP_OK;
}

esp_err_t EspLcdTouchInput::Start(lv_display_t* display) {
    if (touch_ == nullptr || executor_ == nullptr || display == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    display_ = display;
    // Clear a report that may already be pending before the callback is
    // registered. After this one-shot synchronization all reads are driven by
    // the controller interrupt.
    const esp_err_t prime_status = executor_->Invoke(buses::I2cExecutor::Priority::kHigh, PrimeEntry, this);
    if (prime_status != ESP_OK) {
        display_ = nullptr;
        return prime_status;
    }
    active_instance_ = this;
    const esp_err_t status = esp_lcd_touch_register_interrupt_callback(touch_, InterruptEntry);
    if (status != ESP_OK) {
        active_instance_ = nullptr;
        display_ = nullptr;
    }
    return status;
}

esp_err_t EspLcdTouchInput::PrimeEntry(void* context) {
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* input = static_cast<EspLcdTouchInput*>(context);
    const esp_err_t read_status = esp_lcd_touch_read_data(input->touch_);
    if (read_status != ESP_OK && read_status != ESP_ERR_INVALID_RESPONSE) {
        return read_status;
    }
    if (read_status == ESP_ERR_INVALID_RESPONSE) {
        return ESP_OK;
    }
    esp_lcd_touch_point_data_t points[MICROPIXEL_MAX_TOUCH_POINTS]{};
    uint8_t point_count = 0U;
    const esp_err_t data_status =
        esp_lcd_touch_get_data(input->touch_, points, &point_count, MICROPIXEL_MAX_TOUCH_POINTS);
    if (data_status != ESP_OK) {
        return data_status;
    }
    return ESP_OK;
}

int32_t EspLcdTouchInput::GetInfo(micropixel_input_info_t& info) {
    if (!Available()) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_INPUT_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_INPUT_INTERFACE_MINOR;
    info.capabilities = 0U;
    info.logical_width = width_;
    info.logical_height = height_;
    info.max_touch_points = MICROPIXEL_MAX_TOUCH_POINTS;
    return MICROPIXEL_STATUS_OK;
}

void EspLcdTouchInput::BindTouchSink(device::TouchSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    sink_ = sink;
    sink_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void EspLcdTouchInput::UnbindTouchSink(void* context) {
    portENTER_CRITICAL(&sink_lock_);
    if (sink_context_ == context) {
        sink_ = nullptr;
        sink_context_ = nullptr;
    }
    portEXIT_CRITICAL(&sink_lock_);
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

bool EspLcdTouchInput::InjectTouch(const device::TouchSample& sample) {
    if (!Available() || sample.x < 0 || sample.y < 0 || sample.x >= width_ || sample.y >= height_ ||
        sample.pressure_per_mille > 1000U) {
        return false;
    }
    Emit(sample);
    return true;
}

void EspLcdTouchInput::Emit(const device::TouchSample& sample) {
    device::TouchSink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    if (sink_ != nullptr) {
        sink = sink_;
        context = sink_context_;
        ++sink_inflight_;
    }
    portEXIT_CRITICAL(&sink_lock_);
    if (sink == nullptr) {
        return;
    }
    (void)sink(context, sample);
    portENTER_CRITICAL(&sink_lock_);
    --sink_inflight_;
    portEXIT_CRITICAL(&sink_lock_);
}

void IRAM_ATTR EspLcdTouchInput::InterruptEntry(esp_lcd_touch_handle_t) {
    EspLcdTouchInput* instance = active_instance_;
    if (instance == nullptr) {
        return;
    }
    instance->interrupts_.fetch_add(1U, std::memory_order_relaxed);
    if (instance->executor_ == nullptr || instance->work_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (!instance->executor_->PostFromIsr(buses::I2cExecutor::Priority::kHigh, ProcessEntry, instance,
                                          &higher_priority_task_woken)) {
        instance->work_pending_.store(false, std::memory_order_release);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t EspLcdTouchInput::ProcessEntry(void* context) {
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    static_cast<EspLcdTouchInput*>(context)->ProcessInterrupt();
    return ESP_OK;
}

void EspLcdTouchInput::ProcessInterrupt() {
    const uint32_t observed_interrupts = interrupts_.load(std::memory_order_acquire);
    bool sample_decoded = false;
    const esp_err_t read_status = esp_lcd_touch_read_data(touch_);
    if (read_status == ESP_ERR_INVALID_RESPONSE) {
        // CST9217 can pulse INT for a frame whose ACK byte is not yet valid.
        // Its driver deliberately leaves tp->data unchanged in that case, and
        // the official LVGL adapter likewise keeps the previous press state.
        // Do not synthesize Up/Cancel here: the next valid controller report
        // will either advance the same track or release it.
        sample_decoded = true;
    } else if (read_status != ESP_OK) {
        ESP_LOGW(kTag, "touch sample read failed: %s", esp_err_to_name(read_status));
    } else {
        esp_lcd_touch_point_data_t points[MICROPIXEL_MAX_TOUCH_POINTS]{};
        uint8_t point_count = 0U;
        const esp_err_t data_status = esp_lcd_touch_get_data(touch_, points, &point_count, MICROPIXEL_MAX_TOUCH_POINTS);
        if (data_status != ESP_OK) {
            ESP_LOGW(kTag, "touch sample decode failed: %s", esp_err_to_name(data_status));
        } else {
            sample_decoded = true;
            const uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            bool previous_seen[MICROPIXEL_MAX_TOUCH_POINTS]{};
            for (uint8_t point_index = 0U; point_index < point_count; ++point_index) {
                auto& point = points[point_index];
                // CST9217 occasionally reports the exclusive panel extent
                // (480) at the physical edge. DeviceServices coordinates are
                // always inside [0, extent), so normalize before routing.
                point.x = std::min<uint16_t>(point.x, static_cast<uint16_t>(width_ - 1));
                point.y = std::min<uint16_t>(point.y, static_cast<uint16_t>(height_ - 1));
                int32_t previous_index = -1;
                for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
                    if (active_touches_[index].active && active_touches_[index].point.track_id == point.track_id) {
                        previous_index = static_cast<int32_t>(index);
                        previous_seen[index] = true;
                        break;
                    }
                }
                Emit({.timestamp_us = timestamp_us,
                      .id = point.track_id,
                      .x = point.x,
                      .y = point.y,
                      .pressure_per_mille = 0U,
                      .phase = previous_index >= 0 ? device::TouchPhase::kMove : device::TouchPhase::kDown});
            }
            for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
                if (!active_touches_[index].active || previous_seen[index]) {
                    continue;
                }
                const auto& point = active_touches_[index].point;
                Emit({.timestamp_us = timestamp_us,
                      .id = point.track_id,
                      .x = point.x,
                      .y = point.y,
                      .pressure_per_mille = 0U,
                      .phase = device::TouchPhase::kUp});
            }
            for (auto& active : active_touches_) {
                active = {};
            }
            for (uint8_t index = 0U; index < point_count; ++index) {
                active_touches_[index] = {.active = true, .point = points[index]};
            }
        }
    }
    if (!sample_decoded) {
        const uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        for (auto& active : active_touches_) {
            if (active.active) {
                Emit({.timestamp_us = timestamp_us,
                      .id = active.point.track_id,
                      .x = active.point.x,
                      .y = active.point.y,
                      .pressure_per_mille = 0U,
                      .phase = device::TouchPhase::kCancel});
            }
            active = {};
        }
    }

    work_pending_.store(false, std::memory_order_release);
    if (interrupts_.load(std::memory_order_acquire) != observed_interrupts) {
        if (!work_pending_.exchange(true, std::memory_order_acq_rel) &&
            !executor_->Post(buses::I2cExecutor::Priority::kHigh, ProcessEntry, this)) {
            work_pending_.store(false, std::memory_order_release);
        }
    }
}

}  // namespace micropixel::platform::input
