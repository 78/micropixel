#include "sdk/application.hpp"

#include "runtime/direct_surface_state.hpp"
#include "runtime/display_context.hpp"
#include "runtime/service_binding.hpp"

using micropixel::runtime::CopyBytes;
using micropixel::runtime::LoadInputInfo;
using micropixel::runtime::ReleaseSurfaceBuffer;
using micropixel::runtime::RequireOk;
using micropixel::runtime::ToLogical;

namespace micropixel {

// Service capabilities are resolved lazily by the operation that needs them.
// This keeps non-graphical Guests usable on headless bring-up profiles while
// Renderer and touch-coordinate paths still validate the display contract.
Application::Application() noexcept = default;

void Application::BeginRun() const {
    if (running_) {
        runtime::Panic("application.run.reentrant", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    running_ = true;
}

void Application::EndRun() const { running_ = false; }

Event Application::WaitEvent() const {
    Event event;
    if (!WaitEventInternal(event, UINT64_MAX)) {
        runtime::Panic("application.wait_event.timeout", MICROPIXEL_STATUS_INTERNAL);
    }
    return event;
}

bool Application::WaitEventFor(Event& event, Duration timeout) const {
    if (timeout.count_microseconds() == UINT64_MAX) {
        runtime::Panic("application.wait_event_for.timeout", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return WaitEventInternal(event, timeout.count_microseconds());
}

bool Application::PollEvent(Event& event) const { return WaitEventInternal(event, 0U); }

bool Application::WaitEventInternal(Event& event, uint64_t timeout_us) const {
    micropixel_event_t raw{};
    const int32_t status = micropixel_event_wait(&raw, sizeof(raw), timeout_us);
    if (status == MICROPIXEL_STATUS_TIMEOUT) {
        return false;
    }
    RequireOk(status, "application.wait_event");
    if (raw.size != sizeof(raw)) {
        runtime::Panic("application.wait_event.size", MICROPIXEL_STATUS_INTERNAL);
    }

    TimePoint timestamp{raw.timestamp_us};
    if (raw.service_id == 0U && raw.event_id == MICROPIXEL_CORE_EVENT_RESUME) {
        event = Event{EventType::kResume, timestamp};
        return true;
    }
    if (raw.service_id == 0U && raw.event_id == MICROPIXEL_CORE_EVENT_STOP) {
        event = Event{EventType::kStop, timestamp};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_TIMER && raw.event_id == MICROPIXEL_TIMER_EVENT_EXPIRED) {
        micropixel_timer_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        event =
            Event{TimerEvent{timestamp, Duration::Microseconds(payload.elapsed_us), payload.missed_count, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_AUDIO && raw.event_id == MICROPIXEL_AUDIO_EVENT_PLAYBACK_FINISHED) {
        micropixel_audio_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.playback == 0U || payload.playback != raw.source || payload.reserved[0] != 0U ||
            payload.reserved[1] != 0U || payload.reserved[2] != 0U || raw.status > MICROPIXEL_STATUS_OK) {
            runtime::Panic("application.wait_event.audio_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{AudioPlaybackEvent{timestamp, raw.status == MICROPIXEL_STATUS_OK, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_AUDIO && raw.event_id == MICROPIXEL_AUDIO_EVENT_PCM_STREAM_LOW_WATER) {
        micropixel_audio_pcm_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.stream == 0U || payload.stream != raw.source || payload.reserved[0] != 0U ||
            payload.reserved[1] != 0U || raw.status != MICROPIXEL_STATUS_OK) {
            runtime::Panic("application.wait_event.pcm_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{PcmStreamEvent{timestamp, payload.free_frames, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_GRAPHICS && raw.event_id == MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED) {
        micropixel_surface_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.surface == 0U || payload.surface != raw.source ||
            payload.buffer_index >= MICROPIXEL_SURFACE_MAX_BUFFERS || raw.status != MICROPIXEL_STATUS_OK) {
            runtime::Panic("application.wait_event.surface_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        // Releases for a surface that was already destroyed may still be queued;
        // they are delivered but do not touch the live surface's ownership.
        ReleaseSurfaceBuffer(payload.surface, payload.buffer_index);
        event = Event{SurfaceReleasedEvent{timestamp, payload.buffer_index, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_DEVICES &&
        (raw.event_id == MICROPIXEL_DEVICES_EVENT_ADDED || raw.event_id == MICROPIXEL_DEVICES_EVENT_REMOVED)) {
        micropixel_device_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.device == 0U || payload.kind == MICROPIXEL_DEVICE_KIND_ANY ||
            payload.kind > MICROPIXEL_DEVICE_KIND_NETWORK || payload.reserved0 != 0U || payload.reserved1 != 0U) {
            runtime::Panic("application.wait_event.device_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        const EventType type =
            raw.event_id == MICROPIXEL_DEVICES_EVENT_ADDED ? EventType::kDeviceAdded : EventType::kDeviceRemoved;
        event = Event{type, DeviceEvent{timestamp, DeviceId{payload.device}, static_cast<DeviceKind>(payload.kind),
                                        payload.generation}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_GPIO && raw.event_id == MICROPIXEL_GPIO_EVENT_EDGE) {
        micropixel_gpio_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (raw.source == 0U || payload.device == 0U || payload.value > 1U ||
            (payload.edge != MICROPIXEL_GPIO_EDGE_RISING && payload.edge != MICROPIXEL_GPIO_EDGE_FALLING) ||
            payload.reserved0 != 0U) {
            runtime::Panic("application.wait_event.gpio_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{GpioEdgeEvent{timestamp, DeviceId{payload.device}, payload.value != 0U,
                                    static_cast<GpioEdge>(payload.edge), raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_HAPTICS && raw.event_id == MICROPIXEL_HAPTICS_EVENT_FINISHED) {
        if (raw.source == 0U) {
            runtime::Panic("application.wait_event.haptics_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{HapticEvent{timestamp, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_INPUT && raw.event_id == MICROPIXEL_INPUT_EVENT_TOUCH) {
        micropixel_touch_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        TouchPhase phase = TouchPhase::kCancel;
        switch (payload.phase) {
            case MICROPIXEL_TOUCH_DOWN:
                phase = TouchPhase::kDown;
                break;
            case MICROPIXEL_TOUCH_MOVE:
                phase = TouchPhase::kMove;
                break;
            case MICROPIXEL_TOUCH_UP:
                phase = TouchPhase::kUp;
                break;
            case MICROPIXEL_TOUCH_CANCEL:
                phase = TouchPhase::kCancel;
                break;
            default:
                runtime::Panic("application.wait_event.touch_phase", MICROPIXEL_STATUS_INTERNAL);
        }
        const micropixel_input_info_t& input = LoadInputInfo();
        const bool has_pressure = (input.capabilities & MICROPIXEL_INPUT_CAP_PRESSURE) != 0U;
        if (has_pressure && payload.pressure_per_mille > 1000U) {
            runtime::Panic("application.wait_event.touch_pressure", MICROPIXEL_STATUS_INTERNAL);
        }
        const Point logical = ToLogical(Point{payload.x, payload.y});
        event = Event{TouchEvent{timestamp, phase, raw.source, logical.x, logical.y, has_pressure,
                                 static_cast<uint16_t>(has_pressure ? payload.pressure_per_mille : 0U)}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_INPUT && raw.event_id == MICROPIXEL_INPUT_EVENT_KEY) {
        micropixel_key_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.code < MICROPIXEL_KEY_UP || payload.code > MICROPIXEL_KEY_GAMEPAD_NORTH ||
            payload.modifiers != 0U || payload.reserved0 != 0U) {
            runtime::Panic("application.wait_event.key_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        KeyPhase phase = KeyPhase::kCancel;
        switch (payload.phase) {
            case MICROPIXEL_KEY_DOWN_PHASE:
                phase = KeyPhase::kDown;
                break;
            case MICROPIXEL_KEY_UP_PHASE:
                phase = KeyPhase::kUp;
                break;
            case MICROPIXEL_KEY_REPEAT_PHASE:
                phase = KeyPhase::kRepeat;
                break;
            case MICROPIXEL_KEY_CANCEL_PHASE:
                phase = KeyPhase::kCancel;
                break;
            default:
                runtime::Panic("application.wait_event.key_phase", MICROPIXEL_STATUS_INTERNAL);
        }
        if ((phase == KeyPhase::kRepeat) != (payload.repeat_count != 0U)) {
            runtime::Panic("application.wait_event.key_repeat", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{KeyEvent{timestamp, static_cast<KeyCode>(payload.code), phase, payload.repeat_count}};
        return true;
    }

    event = Event{timestamp};
    return true;
}

}  // namespace micropixel
