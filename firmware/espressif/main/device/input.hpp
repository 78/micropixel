#ifndef MICROPIXEL_DEVICE_INPUT_HPP
#define MICROPIXEL_DEVICE_INPUT_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

enum class TouchPhase : uint8_t {
    kDown,
    kMove,
    kUp,
    kCancel,
};

struct TouchSample final {
    uint64_t timestamp_us{};
    uint32_t id{};
    uint16_t x{};
    uint16_t y{};
    uint16_t pressure{};
    TouchPhase phase{TouchPhase::kCancel};
};

using TouchSink = bool (*)(void* context, const TouchSample& sample);

class InputBackend {
   public:
    virtual ~InputBackend() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_input_info_t& info) = 0;
    virtual void BindTouchSink(TouchSink sink, void* context) = 0;
    virtual void UnbindTouchSink(void* context) = 0;
};

}  // namespace micropixel::device

#endif
