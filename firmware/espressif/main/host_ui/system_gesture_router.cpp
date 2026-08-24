#include "host_ui/system_gesture_router.hpp"

#include <cstdlib>

#include "esp_log.h"
#include "freertos/task.h"

namespace micropixel::host_ui {
namespace {

constexpr char kTag[] = "micropixel_gestures";
constexpr uint16_t kTopReservedEdgeHeight = 64U;
constexpr uint16_t kBottomReservedEdgeHeight = 32U;
constexpr int32_t kTopRecognitionDistance = 56;
constexpr int32_t kBottomRecognitionDistance = 56;
constexpr int32_t kDirectionSlop = 40;
constexpr uint64_t kRecognitionTimeoutUs = 600000U;
constexpr uint64_t kPostGestureReleaseGuardUs = 300000U;

}  // namespace

SystemGestureRouter::SystemGestureRouter(device::InputBackend& input, uint16_t width, uint16_t height)
    : input_(input), width_(width), height_(height) {
    input_.BindTouchSink(Receive, this);
}

SystemGestureRouter::~SystemGestureRouter() { input_.UnbindTouchSink(this); }

int32_t SystemGestureRouter::GetInfo(micropixel_input_info_t& info) { return input_.GetInfo(info); }

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
    if (!candidate_.active && sample.timestamp_us < quarantine_until_us_) {
        return true;
    }
    if (sample.phase == device::TouchPhase::kDown && !candidate_.active) {
        if (sample.x < 0 || sample.y < 0 || sample.x >= width_ || sample.y >= height_) {
            return Forward(sample);
        }
        Edge edge = Edge::kNone;
        if (sample.y < kTopReservedEdgeHeight) {
            edge = Edge::kTop;
        } else if (sample.y >= height_ - kBottomReservedEdgeHeight) {
            edge = Edge::kBottom;
        }
        if (edge == Edge::kNone) {
            return Forward(sample);
        }
        candidate_ = Candidate{.initial = sample, .edge = edge, .active = true};
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
    const bool correct_direction =
        candidate_.edge == Edge::kTop ? delta_y >= kTopRecognitionDistance : delta_y <= -kBottomRecognitionDistance;
    if (sample.phase == device::TouchPhase::kMove && correct_direction && std::abs(delta_x) <= kDirectionSlop &&
        elapsed_us <= kRecognitionTimeoutUs) {
        candidate_.recognized = true;
        Emit(candidate_.edge == Edge::kTop ? SystemUiActionType::kOpenStatusLayer : SystemUiActionType::kSuspendToHall,
             sample.timestamp_us);
        return true;
    }

    const bool rejected = std::abs(delta_x) > kDirectionSlop || elapsed_us > kRecognitionTimeoutUs ||
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
    const bool replay_latest_move = candidate_.has_latest_move && (sample.phase != device::TouchPhase::kMove ||
                                                                   sample.timestamp_us != latest_move.timestamp_us);
    ClearCandidate();
    const bool initial_forwarded = Forward(initial);
    const bool move_forwarded = !replay_latest_move || Forward(latest_move);
    const bool sample_forwarded = Forward(sample);
    return initial_forwarded && move_forwarded && sample_forwarded;
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
