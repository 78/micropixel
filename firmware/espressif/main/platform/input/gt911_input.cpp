#include "platform/input/gt911_input.hpp"

#include <cstdio>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::input {
namespace {

constexpr char kTag[] = "micropixel_touch";
constexpr int kI2cSpeedHz = 400000;

}  // namespace

Gt911Input* Gt911Input::active_instance = nullptr;

Gt911Input::Gt911Input(int32_t width, int32_t height, gpio_num_t interrupt_pin)
    : width_(width), height_(height), interrupt_pin_(interrupt_pin) {}

uint8_t Gt911Input::ProbeAddress(i2c_master_bus_handle_t i2c_bus) const {
    constexpr uint8_t kAddresses[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };
    for (uint8_t address : kAddresses) {
        if (i2c_master_probe(i2c_bus, address, 100) == ESP_OK) {
            ESP_LOGI(kTag, "GT911 found at I2C address 0x%02x", address);
            return address;
        }
    }
    ESP_LOGW(kTag, "GT911 probe failed; trying the default address");
    return ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
}

esp_err_t Gt911Input::Initialize(i2c_master_bus_handle_t i2c_bus, buses::I2cExecutor& i2c_executor) {
    struct Request final {
        Gt911Input* input;
        i2c_master_bus_handle_t bus;
    } request{this, i2c_bus};
    i2c_executor_ = &i2c_executor;
    return i2c_executor.Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            return requested.input->InitializeOnWorker(requested.bus);
        },
        &request);
}

esp_err_t Gt911Input::InitializeOnWorker(i2c_master_bus_handle_t i2c_bus) {
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ProbeAddress(i2c_bus);
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.lcd_param_bits = 0;
    io_config.scl_speed_hz = kI2cSpeedHz;
    io_config.flags.disable_control_phase = true;

    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_err_t status = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &touch_io);
    if (status != ESP_OK) {
        return status;
    }

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = width_;
    touch_config.y_max = height_;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = interrupt_pin_;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    touch_config.flags.swap_xy = false;
    touch_config.flags.mirror_x = false;
    touch_config.flags.mirror_y = false;
    return esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch_);
}

esp_err_t Gt911Input::Start(lv_display_t* display) {
    if (touch_ == nullptr || display == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    display_ = display;
    active_instance = this;
    const esp_err_t status = esp_lcd_touch_register_interrupt_callback(touch_, InterruptEntry);
    if (status != ESP_OK) {
        active_instance = nullptr;
    }
    return status;
}

int32_t Gt911Input::GetInfo(micropixel_input_info_t& info) {
    if (!Available()) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_INPUT_INTERFACE_MAJOR;
    info.interface_minor = 0U;
    /* GT911 reports contact area/strength, not calibrated touch pressure. */
    info.capabilities = 0U;
    info.logical_width = width_;
    info.logical_height = height_;
    info.max_touch_points = MICROPIXEL_MAX_TOUCH_POINTS;
    return MICROPIXEL_STATUS_OK;
}

void Gt911Input::BindTouchSink(device::TouchSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    sink_context_ = context;
    sink_ = sink;
    portEXIT_CRITICAL(&sink_lock_);
}

void Gt911Input::UnbindTouchSink(void* context) {
    portENTER_CRITICAL(&sink_lock_);
    if (sink_context_ == context) {
        sink_ = nullptr;
        sink_context_ = nullptr;
    }
    portEXIT_CRITICAL(&sink_lock_);

    while (true) {
        portENTER_CRITICAL(&sink_lock_);
        uint32_t inflight = sink_inflight_;
        portEXIT_CRITICAL(&sink_lock_);
        if (inflight == 0U) {
            break;
        }
        vTaskDelay(1);
    }
}

void Gt911Input::BindSmokeUi(lv_obj_t* root, lv_obj_t* marker, lv_obj_t* status) {
    smoke_root_ = root;
    smoke_marker_ = marker;
    smoke_status_ = status;
}

void Gt911Input::ClearSmokeUi() {
    smoke_root_ = nullptr;
    smoke_marker_ = nullptr;
    smoke_status_ = nullptr;
}

void Gt911Input::InjectTouchForCapture(const device::TouchSample& sample) { Emit(sample); }

bool Gt911Input::InjectTouch(const device::TouchSample& sample) {
    if (!Available() || sample.x < 0 || sample.y < 0 || sample.x >= width_ || sample.y >= height_ ||
        sample.pressure_per_mille > 1000U) {
        return false;
    }
    Emit(sample);
    return true;
}

void Gt911Input::Emit(const device::TouchSample& sample) {
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

void IRAM_ATTR Gt911Input::InterruptEntry(esp_lcd_touch_handle_t) {
    Gt911Input* instance = active_instance;
    if (instance == nullptr) {
        return;
    }
    instance->interrupts_.fetch_add(1, std::memory_order_relaxed);
    if (instance->i2c_executor_ == nullptr || instance->work_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (!instance->i2c_executor_->PostFromIsr(buses::I2cExecutor::Priority::kHigh, ProcessEntry, instance,
                                              &higher_priority_task_woken)) {
        instance->work_pending_.store(false, std::memory_order_release);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t Gt911Input::ProcessEntry(void* context) {
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    static_cast<Gt911Input*>(context)->ProcessInterrupt();
    return ESP_OK;
}

void Gt911Input::UpdateSmokeUi(const esp_lcd_touch_point_data_t* points, uint8_t point_count) {
    portENTER_CRITICAL(&sink_lock_);
    bool has_sink = sink_ != nullptr;
    portEXIT_CRITICAL(&sink_lock_);
    if (has_sink || esp_lv_adapter_lock(0) != ESP_OK) {
        return;
    }
    if (smoke_root_ != nullptr && smoke_marker_ != nullptr && smoke_status_ != nullptr) {
        if (point_count == 0U) {
            lv_obj_add_flag(smoke_marker_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(smoke_status_, "Touch released");
        } else {
            lv_obj_remove_flag(smoke_marker_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(smoke_marker_, points[0].x - 14, points[0].y - 14);
            char text[80]{};
            std::snprintf(text, sizeof(text), "Touch x=%u y=%u id=%u irq=%lu", points[0].x, points[0].y,
                          points[0].track_id, static_cast<unsigned long>(interrupts_.load(std::memory_order_relaxed)));
            lv_label_set_text(smoke_status_, text);
        }
        lvgl::RequestDisplayRefresh(display_);
    }
    esp_lv_adapter_unlock();
}

void Gt911Input::ProcessInterrupt() {
    const uint32_t observed_interrupts = interrupts_.load(std::memory_order_acquire);
    esp_err_t read_status = esp_lcd_touch_read_data(touch_);
    if (read_status != ESP_OK) {
        ESP_LOGW(kTag, "GT911 sample read failed: %s", esp_err_to_name(read_status));
    } else {
        esp_lcd_touch_point_data_t points[MICROPIXEL_MAX_TOUCH_POINTS]{};
        uint8_t point_count = 0U;
        esp_err_t data_status = esp_lcd_touch_get_data(touch_, points, &point_count, MICROPIXEL_MAX_TOUCH_POINTS);
        if (data_status != ESP_OK) {
            ESP_LOGW(kTag, "GT911 sample decode failed: %s", esp_err_to_name(data_status));
        } else {
            uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            bool previous_seen[MICROPIXEL_MAX_TOUCH_POINTS]{};
            for (uint8_t point_index = 0U; point_index < point_count; ++point_index) {
                const auto& point = points[point_index];
                int32_t previous_index = -1;
                for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
                    if (active_touches_[index].active && active_touches_[index].point.track_id == point.track_id) {
                        previous_index = static_cast<int32_t>(index);
                        previous_seen[index] = true;
                        break;
                    }
                }

                device::TouchSample sample{};
                sample.timestamp_us = timestamp_us;
                sample.id = point.track_id;
                sample.x = point.x;
                sample.y = point.y;
                sample.pressure_per_mille = 0U;
                sample.phase = previous_index >= 0 ? device::TouchPhase::kMove : device::TouchPhase::kDown;
                Emit(sample);
            }

            for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
                if (!active_touches_[index].active || previous_seen[index]) {
                    continue;
                }
                const auto& point = active_touches_[index].point;
                device::TouchSample sample{};
                sample.timestamp_us = timestamp_us;
                sample.id = point.track_id;
                sample.x = point.x;
                sample.y = point.y;
                sample.phase = device::TouchPhase::kUp;
                Emit(sample);
            }

            for (auto& active : active_touches_) {
                active = {};
            }
            for (uint8_t index = 0U; index < point_count; ++index) {
                active_touches_[index].active = true;
                active_touches_[index].point = points[index];
            }
            UpdateSmokeUi(points, point_count);
        }
    }

    work_pending_.store(false, std::memory_order_release);
    if (interrupts_.load(std::memory_order_acquire) != observed_interrupts &&
        !work_pending_.exchange(true, std::memory_order_acq_rel) &&
        !i2c_executor_->Post(buses::I2cExecutor::Priority::kHigh, ProcessEntry, this)) {
        work_pending_.store(false, std::memory_order_release);
    }
}

}  // namespace micropixel::platform::input
