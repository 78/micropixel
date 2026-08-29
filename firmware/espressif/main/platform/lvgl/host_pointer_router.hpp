#ifndef MICROPIXEL_PLATFORM_LVGL_HOST_POINTER_ROUTER_HPP
#define MICROPIXEL_PLATFORM_LVGL_HOST_POINTER_ROUTER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl {

template <size_t QueueCapacity>
class HostPointerRouter final {
   public:
    static_assert(QueueCapacity >= 2U);
    using PointerReleasedSink = void (*)(void* context);

    // Must be called while the caller owns the LVGL lock. The router object
    // must then remain alive for at least as long as the created indev.
    [[nodiscard]] esp_err_t InitializeLocked(lv_display_t* display, PointerReleasedSink released_sink,
                                             void* released_context) {
        if (display == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (indev_ != nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        display_ = display;
        released_sink_ = released_sink;
        released_context_ = released_context;
        indev_ = lv_indev_create();
        if (indev_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_mode(indev_, LV_INDEV_MODE_EVENT);
        lv_indev_set_read_cb(indev_, Read);
        lv_indev_set_user_data(indev_, this);
        lv_indev_set_display(indev_, display_);
        SetEnabledLocked(false);
        return ESP_OK;
    }

    [[nodiscard]] bool Inject(const device::TouchSample& sample) {
        portENTER_CRITICAL(&lock_);
        if (!enabled_) {
            portEXIT_CRITICAL(&lock_);
            return true;
        }
        if (sample.phase == device::TouchPhase::kDown && !touch_active_) {
            touch_active_ = true;
            touch_id_ = sample.id;
        } else if (!touch_active_ || sample.id != touch_id_) {
            portEXIT_CRITICAL(&lock_);
            return true;
        }
        const uint32_t next_head = (queue_head_ + 1U) % QueueCapacity;
        if (next_head == queue_tail_) {
            queue_tail_ = (queue_tail_ + 1U) % QueueCapacity;
        }
        queue_[queue_head_] = sample;
        queue_head_ = next_head;
        if (sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel) {
            touch_active_ = false;
            release_queued_ = true;
        }
        portEXIT_CRITICAL(&lock_);
        if (indev_ != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_timer_t* read_timer = lv_indev_get_read_timer(indev_);
            if (read_timer != nullptr) {
                lv_timer_resume(read_timer);
                lv_timer_ready(read_timer);
            }
            esp_lv_adapter_unlock();
        }
        (void)esp_lv_adapter_request_wake();
        return true;
    }

    void SetEnabledLocked(bool enabled) {
        portENTER_CRITICAL(&lock_);
        queue_head_ = 0U;
        queue_tail_ = 0U;
        pointer_state_ = LV_INDEV_STATE_RELEASED;
        touch_active_ = false;
        release_queued_ = false;
        enabled_ = enabled;
        portEXIT_CRITICAL(&lock_);
        if (indev_ != nullptr) {
            lv_indev_set_mode(indev_, LV_INDEV_MODE_EVENT);
            lv_indev_enable(indev_, enabled);
            lv_indev_reset(indev_, nullptr);
        }
    }

    [[nodiscard]] bool Busy() {
        portENTER_CRITICAL(&lock_);
        const bool busy = touch_active_ || release_queued_ || queue_head_ != queue_tail_;
        portEXIT_CRITICAL(&lock_);
        return busy;
    }

    [[nodiscard]] lv_indev_t* indev() const { return indev_; }

   private:
    static void Read(lv_indev_t* indev, lv_indev_data_t* data) {
        auto* router = static_cast<HostPointerRouter*>(lv_indev_get_user_data(indev));
        if (router == nullptr || data == nullptr) {
            return;
        }
        bool sample_read = false;
        bool pointer_released = false;
        portENTER_CRITICAL(&router->lock_);
        if (router->queue_tail_ != router->queue_head_) {
            const device::TouchSample& sample = router->queue_[router->queue_tail_];
            router->queue_tail_ = (router->queue_tail_ + 1U) % QueueCapacity;
            router->point_ = {.x = sample.x, .y = sample.y};
            router->pointer_state_ =
                sample.phase == device::TouchPhase::kDown || sample.phase == device::TouchPhase::kMove
                    ? LV_INDEV_STATE_PRESSED
                    : LV_INDEV_STATE_RELEASED;
            if (sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel) {
                router->release_queued_ = false;
                pointer_released = true;
            }
            sample_read = true;
        }
        data->point = router->point_;
        data->state = router->enabled_ ? router->pointer_state_ : LV_INDEV_STATE_RELEASED;
        data->continue_reading = router->queue_tail_ != router->queue_head_;
        portEXIT_CRITICAL(&router->lock_);
        if (sample_read && router->display_ != nullptr) {
            RequestDisplayRefresh(router->display_);
        }
        if (pointer_released && router->released_sink_ != nullptr) {
            router->released_sink_(router->released_context_);
        }
    }

    lv_display_t* display_{};
    lv_indev_t* indev_{};
    PointerReleasedSink released_sink_{};
    void* released_context_{};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    std::array<device::TouchSample, QueueCapacity> queue_{};
    lv_point_t point_{};
    lv_indev_state_t pointer_state_{LV_INDEV_STATE_RELEASED};
    uint32_t touch_id_{};
    uint32_t queue_head_{};
    uint32_t queue_tail_{};
    bool enabled_{};
    bool touch_active_{};
    bool release_queued_{};
};

}  // namespace micropixel::platform::lvgl

#endif
