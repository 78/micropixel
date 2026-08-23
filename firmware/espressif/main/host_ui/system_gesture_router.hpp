#ifndef MICROPIXEL_HOST_UI_SYSTEM_GESTURE_ROUTER_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_GESTURE_ROUTER_HPP

#include <cstdint>

#include "device/input.hpp"
#include "freertos/FreeRTOS.h"
#include "host_ui/system_ui.hpp"

namespace micropixel::host_ui {

// Permanently owns the raw platform touch sink. Edge candidates are held
// until accepted or rejected so reserved System gestures never leak a partial
// Down/Move sequence into the Guest.
class SystemGestureRouter final : public device::InputBackend {
   public:
    SystemGestureRouter(device::InputBackend& input, uint16_t width, uint16_t height);
    SystemGestureRouter(const SystemGestureRouter&) = delete;
    SystemGestureRouter& operator=(const SystemGestureRouter&) = delete;
    ~SystemGestureRouter() override;

    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override;
    void BindTouchSink(device::TouchSink sink, void* context) override;
    void UnbindTouchSink(void* context) override;

    void BindSystemActionSink(SystemUiActionSink sink, void* context);
    void ClearSystemActionSink(void* context);

   private:
    enum class Edge : uint8_t {
        kNone,
        kTop,
        kBottom,
    };

    struct Candidate final {
        device::TouchSample initial{};
        device::TouchSample latest_move{};
        uint32_t replacement_ids[MICROPIXEL_MAX_TOUCH_POINTS]{};
        Edge edge{Edge::kNone};
        uint8_t replacement_count{};
        bool active{};
        bool recognized{};
        bool initial_released{};
        bool has_latest_move{};
    };

    static bool Receive(void* context, const device::TouchSample& sample);
    [[nodiscard]] bool Route(const device::TouchSample& sample);
    [[nodiscard]] bool Forward(const device::TouchSample& sample);
    void Emit(SystemUiActionType type);
    void ClearCandidate();

    device::InputBackend& input_;
    uint16_t width_{};
    uint16_t height_{};
    portMUX_TYPE sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    device::TouchSink downstream_sink_{};
    void* downstream_context_{};
    uint32_t downstream_inflight_{};
    SystemUiActionSink system_sink_{};
    void* system_context_{};
    Candidate candidate_{};
    uint64_t quarantine_until_us_{};
};

}  // namespace micropixel::host_ui

#endif
