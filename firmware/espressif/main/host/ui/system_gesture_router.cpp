#include "host/ui/system_gesture_router.hpp"

#include <cinttypes>
#include <cstdlib>

#include "esp_log.h"
#include "freertos/task.h"
#include "host/ui/gesture_thresholds.hpp"

namespace micropixel::host_ui {
namespace {

constexpr char kTag[] = "micropixel_gestures";
constexpr uint64_t kRecognitionTimeoutUs = 600000U;
constexpr uint64_t kPostGestureReleaseGuardUs = 300000U;

bool InBottomCenterThird(int32_t x, uint16_t width) {
    const int32_t side_width = static_cast<int32_t>(width) / 3;
    return x >= side_width && x < static_cast<int32_t>(width) - side_width;
}

}  // namespace

SystemGestureRouter::SystemGestureRouter(device::Input& input, uint16_t width, uint16_t height)
    : input_(input), width_(width), height_(height) {
    input_.BindTouchSink(Receive, this);
}

SystemGestureRouter::~SystemGestureRouter() { input_.UnbindTouchSink(this); }

int32_t SystemGestureRouter::GetInfo(micropixel_input_info_t& info) {
    const int32_t status = input_.GetInfo(info);
    if (status == MICROPIXEL_STATUS_OK) {
        info.interface_major = MICROPIXEL_INPUT_INTERFACE_MAJOR;
        info.interface_minor = MICROPIXEL_INPUT_INTERFACE_MINOR;
        info.capabilities |= MICROPIXEL_INPUT_CAP_KEY_EVENTS;
    }
    return status;
}

void SystemGestureRouter::BindTouchSink(device::TouchSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    downstream_sink_ = sink;
    downstream_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void SystemGestureRouter::UnbindTouchSink(void* context) {
    portENTER_CRITICAL(&sink_lock_);
    if (downstream_context_ == context) {
        downstream_sink_ = nullptr;
        downstream_context_ = nullptr;
    }
    portEXIT_CRITICAL(&sink_lock_);

    for (;;) {
        portENTER_CRITICAL(&sink_lock_);
        const uint32_t inflight = downstream_inflight_;
        portEXIT_CRITICAL(&sink_lock_);
        if (inflight == 0U) {
            break;
        }
        vTaskDelay(1U);
    }
}

bool SystemGestureRouter::InjectTouch(const device::TouchSample& sample) { return input_.InjectTouch(sample); }

void SystemGestureRouter::BindKeySink(device::KeySink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    key_sink_ = sink;
    key_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void SystemGestureRouter::UnbindKeySink(void* context) {
    portENTER_CRITICAL(&sink_lock_);
    if (key_context_ == context) {
        key_sink_ = nullptr;
        key_context_ = nullptr;
    }
    portEXIT_CRITICAL(&sink_lock_);

    for (;;) {
        portENTER_CRITICAL(&sink_lock_);
        const uint32_t inflight = key_inflight_;
        portEXIT_CRITICAL(&sink_lock_);
        if (inflight == 0U) {
            break;
        }
        vTaskDelay(1U);
    }
}

bool SystemGestureRouter::InjectKey(const device::KeySample& sample) {
    NotifyActivity();
    return ForwardKey(sample);
}

void SystemGestureRouter::SetActivitySink(device::InputActivitySink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    activity_sink_ = sink;
    activity_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);

    if (sink != nullptr) {
        return;
    }
    for (;;) {
        portENTER_CRITICAL(&sink_lock_);
        const uint32_t inflight = activity_inflight_;
        portEXIT_CRITICAL(&sink_lock_);
        if (inflight == 0U) {
            break;
        }
        vTaskDelay(1U);
    }
}

void SystemGestureRouter::BindSystemActionSink(SystemUiActionSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    system_sink_ = sink;
    system_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void SystemGestureRouter::ClearSystemActionSink(void* context) {
    portENTER_CRITICAL(&sink_lock_);
    if (system_context_ == context) {
        system_sink_ = nullptr;
        system_context_ = nullptr;
    }
    portEXIT_CRITICAL(&sink_lock_);
}

bool SystemGestureRouter::Receive(void* context, const device::TouchSample& sample) {
    return context != nullptr && static_cast<SystemGestureRouter*>(context)->Route(sample);
}

bool SystemGestureRouter::Route(const device::TouchSample& sample) {
    NotifyActivity();
    if (!candidate_.active && sample.timestamp_us < quarantine_until_us_) {
        return true;
    }
    if (sample.phase == device::TouchPhase::kDown && !candidate_.active) {
        if (sample.x < 0 || sample.y < 0 || sample.x >= width_ || sample.y >= height_) {
            return Forward(sample);
        }
        Edge edge = Edge::kNone;
        const int32_t top_reserved_edge_height =
            gesture_thresholds::ScaleExtent(height_, gesture_thresholds::kTopReservedEdge);
        const int32_t bottom_reserved_edge_height =
            gesture_thresholds::ScaleExtent(height_, gesture_thresholds::kBottomReservedEdge);
        if (sample.y < top_reserved_edge_height) {
            edge = Edge::kTop;
        } else if (sample.y >= static_cast<int32_t>(height_) - bottom_reserved_edge_height &&
                   InBottomCenterThird(sample.x, width_)) {
            edge = Edge::kBottom;
        }
        if (edge == Edge::kNone) {
            return Forward(sample);
        }
        candidate_ = Candidate{.initial = sample, .edge = edge, .active = true};
        ESP_LOGD(kTag, "edge candidate: edge=%s id=%" PRIu32 " x=%d y=%d", edge == Edge::kTop ? "top" : "bottom",
                 sample.id, sample.x, sample.y);
        return true;
    }

    if (candidate_.recognized) {
        // GT911 can replace track_id while the same finger is still down. A
        // recognized edge gesture therefore quarantines every touch sample
        // until both the original and any replacement tracks are released.
        // Otherwise a replacement Down can reach the newly bound Hall/status
        // sink and become an unintended card or quick-setting tap.
        const bool released = sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel;
        if (sample.id == candidate_.initial.id) {
            candidate_.initial_released = candidate_.initial_released || released;
        } else {
            uint8_t replacement_index = candidate_.replacement_count;
            for (uint8_t index = 0U; index < candidate_.replacement_count; ++index) {
                if (candidate_.replacement_ids[index] == sample.id) {
                    replacement_index = index;
                    break;
                }
            }
            if (released) {
                if (replacement_index < candidate_.replacement_count) {
                    --candidate_.replacement_count;
                    candidate_.replacement_ids[replacement_index] =
                        candidate_.replacement_ids[candidate_.replacement_count];
                }
            } else if (replacement_index == candidate_.replacement_count &&
                       candidate_.replacement_count < MICROPIXEL_MAX_TOUCH_POINTS) {
                candidate_.replacement_ids[candidate_.replacement_count++] = sample.id;
            }
        }
        if (candidate_.initial_released && candidate_.replacement_count == 0U) {
            quarantine_until_us_ = sample.timestamp_us + kPostGestureReleaseGuardUs;
            ClearCandidate();
        }
        return true;
    }
    if (!candidate_.active || sample.id != candidate_.initial.id) {
        return Forward(sample);
    }

    const int32_t delta_x = static_cast<int32_t>(sample.x) - candidate_.initial.x;
    const int32_t delta_y = static_cast<int32_t>(sample.y) - candidate_.initial.y;
    const uint64_t elapsed_us = sample.timestamp_us >= candidate_.initial.timestamp_us
                                    ? sample.timestamp_us - candidate_.initial.timestamp_us
                                    : 0U;
    const int32_t recognition_distance =
        gesture_thresholds::ScaleExtent(height_, gesture_thresholds::kSystemRecognitionDistance);
    const int32_t direction_slop = gesture_thresholds::ScaleExtent(width_, gesture_thresholds::kSystemDirectionSlop);
    const bool correct_direction =
        candidate_.edge == Edge::kTop ? delta_y >= recognition_distance : delta_y <= -recognition_distance;
    const bool direction_compatible = std::abs(delta_x) <= std::abs(delta_y) + direction_slop;
    if (sample.phase == device::TouchPhase::kMove && correct_direction && direction_compatible &&
        elapsed_us <= kRecognitionTimeoutUs) {
        candidate_.recognized = true;
        Emit(candidate_.edge == Edge::kTop ? SystemUiActionType::kOpenStatusLayer : SystemUiActionType::kSuspendToHall,
             sample.timestamp_us);
        return true;
    }

    const bool clearly_horizontal =
        std::abs(delta_x) > direction_slop && std::abs(delta_x) > std::abs(delta_y) + direction_slop;
    const bool rejected = clearly_horizontal || elapsed_us > kRecognitionTimeoutUs ||
                          sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel;
    if (!rejected) {
        if (sample.phase == device::TouchPhase::kMove) {
            candidate_.latest_move = sample;
            candidate_.has_latest_move = true;
        }
        return true;
    }

    const device::TouchSample initial = candidate_.initial;
    const device::TouchSample latest_move = candidate_.latest_move;
    ESP_LOGD(kTag, "edge candidate rejected: id=%" PRIu32 " dx=%" PRId32 " dy=%" PRId32 " elapsed=%" PRIu64 " phase=%u",
             sample.id, delta_x, delta_y, elapsed_us, static_cast<unsigned>(sample.phase));
    const bool replay_latest_move = candidate_.has_latest_move && (sample.phase != device::TouchPhase::kMove ||
                                                                   sample.timestamp_us != latest_move.timestamp_us);
    ClearCandidate();
    const bool initial_forwarded = Forward(initial);
    const bool move_forwarded = !replay_latest_move || Forward(latest_move);
    const bool sample_forwarded = Forward(sample);
    return initial_forwarded && move_forwarded && sample_forwarded;
}

void SystemGestureRouter::NotifyActivity() {
    device::InputActivitySink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    if (activity_sink_ != nullptr) {
        sink = activity_sink_;
        context = activity_context_;
        ++activity_inflight_;
    }
    portEXIT_CRITICAL(&sink_lock_);
    if (sink == nullptr) {
        return;
    }
    sink(context);
    portENTER_CRITICAL(&sink_lock_);
    --activity_inflight_;
    portEXIT_CRITICAL(&sink_lock_);
}

bool SystemGestureRouter::Forward(const device::TouchSample& sample) {
    device::TouchSink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    if (downstream_sink_ != nullptr) {
        sink = downstream_sink_;
        context = downstream_context_;
        ++downstream_inflight_;
    }
    portEXIT_CRITICAL(&sink_lock_);
    if (sink == nullptr) {
        return true;
    }
    const bool delivered = sink(context, sample);
    portENTER_CRITICAL(&sink_lock_);
    --downstream_inflight_;
    portEXIT_CRITICAL(&sink_lock_);
    return delivered;
}

bool SystemGestureRouter::ForwardKey(const device::KeySample& sample) {
    device::KeySink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    if (key_sink_ != nullptr) {
        sink = key_sink_;
        context = key_context_;
        ++key_inflight_;
    }
    portEXIT_CRITICAL(&sink_lock_);
    if (sink == nullptr) {
        return false;
    }
    const bool delivered = sink(context, sample);
    portENTER_CRITICAL(&sink_lock_);
    --key_inflight_;
    portEXIT_CRITICAL(&sink_lock_);
    return delivered;
}

void SystemGestureRouter::Emit(SystemUiActionType type, uint64_t timestamp_us) {
    SystemUiActionSink sink = nullptr;
    void* context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    sink = system_sink_;
    context = system_context_;
    portEXIT_CRITICAL(&sink_lock_);
    if (sink != nullptr) {
        sink(context, SystemUiAction{.type = type, .timestamp_us = timestamp_us});
        ESP_LOGI(kTag, "recognized system gesture: action=%u", static_cast<unsigned>(type));
    }
}

void SystemGestureRouter::ClearCandidate() { candidate_ = {}; }

}  // namespace micropixel::host_ui
