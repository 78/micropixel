// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/audio/audio_output_peripheral.hpp"
namespace mpx_port {
class AudioOutput final : public micropixel::platform::audio::AudioOutputPeripheral {
    const char* const name_;
    int16_t samples_[256]{};

   public:
    explicit AudioOutput(const char* name) : name_(name) {}
    esp_err_t Initialize() override;
    esp_err_t Start(int32_t*, uint32_t) override;
    esp_err_t Write(const int32_t*, uint32_t) override;
    esp_err_t Stop() override;
    void Shutdown() override;
    const char* Name() const override { return name_; }
    uint32_t SampleRate() const override { return 48000; }
};
}  // namespace mpx_port
