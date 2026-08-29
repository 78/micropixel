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
    int32_t x{};
    int32_t y{};
    uint16_t pressure_per_mille{};
    TouchPhase phase{TouchPhase::kCancel};
};

using TouchSink = bool (*)(void* context, const TouchSample& sample);

enum class KeyCode : uint16_t {
    kUp = MICROPIXEL_KEY_UP,
    kDown = MICROPIXEL_KEY_DOWN,
    kLeft = MICROPIXEL_KEY_LEFT,
    kRight = MICROPIXEL_KEY_RIGHT,
    kConfirm = MICROPIXEL_KEY_CONFIRM,
    kBack = MICROPIXEL_KEY_BACK,
    kMenu = MICROPIXEL_KEY_MENU,
    kSouth = MICROPIXEL_KEY_GAMEPAD_SOUTH,
    kEast = MICROPIXEL_KEY_GAMEPAD_EAST,
    kWest = MICROPIXEL_KEY_GAMEPAD_WEST,
    kNorth = MICROPIXEL_KEY_GAMEPAD_NORTH,
};

enum class KeyPhase : uint8_t {
    kDown,
    kUp,
    kRepeat,
    kCancel,
};

struct KeySample final {
    uint64_t timestamp_us{};
    KeyCode code{KeyCode::kConfirm};
    KeyPhase phase{KeyPhase::kCancel};
    uint32_t repeat_count{};
};

using KeySink = bool (*)(void* context, const KeySample& sample);
using InputActivitySink = void (*)(void* context);

class Input {
   public:
    virtual ~Input() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_input_info_t& info) = 0;
    virtual void BindTouchSink(TouchSink sink, void* context) = 0;
    virtual void UnbindTouchSink(void* context) = 0;
    virtual void BindKeySink(KeySink sink, void* context) {
        (void)sink;
        (void)context;
    }
    virtual void UnbindKeySink(void* context) { (void)context; }
    virtual void SetActivitySink(InputActivitySink sink, void* context) {
        (void)sink;
        (void)context;
    }
    [[nodiscard]] virtual bool InjectTouch(const TouchSample& sample) {
        (void)sample;
        return false;
    }
    [[nodiscard]] virtual bool InjectKey(const KeySample& sample) {
        (void)sample;
        return false;
    }
};

}  // namespace micropixel::device

#endif
