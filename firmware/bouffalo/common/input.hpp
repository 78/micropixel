// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "device/contracts/input.hpp"
#include "micropixel_mutex.hpp"
namespace mpx_port {
class Input final : public micropixel::device::Input {
    BouffaloMutex mutex_;
    micropixel::device::TouchSink touch_{};
    void* touch_context_{};
    micropixel::device::KeySink key_{};
    void* key_context_{};
    micropixel::device::InputActivitySink activity_{};
    void* activity_context_{};
    micropixel::device::TouchSink host_touch_{};
    void* host_context_{};
    bool active_[16]{};
    int32_t x_[16]{}, y_[16]{};
    bool hardware_{};

   public:
    void BindHostTouch(micropixel::device::TouchSink sink, void* context);
    bool Initialize();
    void Poll();
    int32_t GetInfo(micropixel_input_info_t&) override;
    void BindTouchSink(micropixel::device::TouchSink, void*) override;
    void UnbindTouchSink(void*) override;
    void BindKeySink(micropixel::device::KeySink, void*) override;
    void UnbindKeySink(void*) override;
    void SetActivitySink(micropixel::device::InputActivitySink, void*) override;
    bool InjectTouch(const micropixel::device::TouchSample&) override;
    bool InjectKey(const micropixel::device::KeySample&) override;
};
}  // namespace mpx_port
