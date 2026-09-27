// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::sensecap_watcher {

struct KnobInputConfig final {
    gpio_num_t phase_a;
    gpio_num_t phase_b;
    device::KeyCode clockwise_code{device::KeyCode::kUp};
    device::KeyCode anticlockwise_code{device::KeyCode::kDown};
    // One switch, three gestures. The single click is the primary action, which
    // is the meaning every other MicroPixel board gives its confirm key; the
    // double click steps back. This board first had them the other way round,
    // which put "no" on the gesture people reach for when they mean "yes".
    // A single click cannot be told apart from the first half of a double click
    // until the double-click window closes, so confirm waits that window out:
    // double_click_window_ms is the trade against double-click tolerance.
    // A hold is not a key at all - it is handed to the Host's power path so the
    // Host keeps ownership of the shutdown sequence.
    device::KeyCode single_click_code{device::KeyCode::kConfirm};
    device::KeyCode double_click_code{device::KeyCode::kBack};
    // Reports the switch state. The knob's switch sits on the IO expander
    // rather than a GPIO, so the board supplies the read and this module never
    // touches the control bus itself.
    bool (*read_pressed)(void* context);
    void* read_pressed_context{};
    const char* log_tag;
    const char* task_name;
    BaseType_t task_core;
    uint32_t poll_interval_ms{20U};
    // Quadrature edges per mechanical detent. The decoder below reports every
    // edge, and this wheel is detented: one click is a full quadrature cycle,
    // so without this it reported four steps per click. The Host's focus
    // navigation then jumped four widgets for one click and Guests saw four
    // key presses for one detent, which is what "the knob is far too
    // sensitive" looks like on both sides. Left configurable because a
    // continuous (undetented) encoder spreads the same edges evenly instead.
    uint8_t steps_per_detent{4U};
    // Gesture windows. The hold window matches the long press other MicroPixel
    // boards use to request a power-off. The double-click window is also how
    // long a single click waits to find out that it is not the first half of a
    // pair, so it trades responsiveness against double-click recognition.
    uint32_t double_click_window_ms{300U};
    uint32_t long_press_ms{1000U};
};

// The rotary knob and its centre switch, published through the Input contract
// as key edges. The Input contract has no rotation axis: the vendor maps the
// wheel to device volume, but MicroPixel's Host UI is pointer-driven and the
// Host owns master volume, so a step is reported as a press/release pair that
// Guests and the Host both already understand.
//
// The switch is polled rather than interrupt-driven, because its line sits on
// the IO expander instead of a GPIO. The worker task that polls the quadrature
// phases also samples the switch, so the whole gesture timeline has a single
// clock and a single owner.
class KnobInput final {
   public:
    // Returns true when the Host accepted this hold as the start of a power
    // request, which is the meaning device::Power gives the same callback.
    using LongPressSink = bool (*)(void* context, uint64_t timestamp_us);
    // An optional rotary consumer that wants the raw detents and the confirm
    // gesture rather than key edges, such as LVGL focus navigation. Rotation is
    // reported as a detent count and a confirm as zero steps, because a gesture
    // and a detent can arrive on the same wake-up.
    // A rotary consumer that wants the gestures themselves rather than key
    // edges: rotation as a detent count, and the confirm and back gestures as
    // flags on the call that resolves them. A detent and a gesture can resolve
    // on the same wake-up, so they share one call.
    using NavigationSink = void (*)(void* context, int32_t rotation_steps, bool confirm, bool back);

    explicit KnobInput(KnobInputConfig config) : config_(config) {}
    KnobInput(const KnobInput&) = delete;
    KnobInput& operator=(const KnobInput&) = delete;
    ~KnobInput();

    [[nodiscard]] esp_err_t Initialize(device::Input& input);

    // Both sinks below are installed by the board. Set them before the first
    // gesture; the rotation and switch state they act on is owned by the worker
    // task, so the bindings themselves are protected.
    void SetLongPressSink(LongPressSink sink, void* context);
    void SetNavigationSink(NavigationSink sink, void* context);

   private:
    static void IRAM_ATTR OnEdge(void* context);
    static void WorkerEntry(void* context);
    void Worker();
    void EmitRotation(int32_t steps);
    void EmitClick(device::KeyCode code);
    void NotifyNavigation(int32_t rotation_steps, bool confirm, bool back);
    // Advances the switch gesture state machine. Called from the worker task
    // only, once per rotation edge or poll tick.
    void UpdateButton(bool pressed, uint64_t now_us);
    void RequestPowerOff(uint64_t timestamp_us);
    void Stop();

    KnobInputConfig config_;
    device::Input* input_{};
    TaskHandle_t worker_{};
    std::atomic<bool> stopping_{};
    std::atomic<int32_t> rotation_steps_{};
    // Raw quadrature edges that have not yet added up to a whole detent, owned
    // by the worker task. A click that straddles two wake-ups is neither lost
    // nor counted twice.
    int32_t rotation_remainder_{};
    uint8_t previous_phase_{};
    // Gesture state, owned by the worker task.
    bool button_pressed_{};
    uint64_t press_started_us_{};
    uint64_t click_released_us_{};
    uint8_t clicks_in_window_{};
    bool long_press_fired_{};
    // Bound by the Host task, called by the worker task.
    portMUX_TYPE sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    LongPressSink long_press_sink_{};
    void* long_press_context_{};
    uint32_t sink_inflight_{};
    NavigationSink navigation_sink_{};
    void* navigation_context_{};
};

}  // namespace micropixel::platform::sensecap_watcher
