#include "platform/audio_backend.hpp"

namespace micropixel::platform {
namespace {

class NullAudioBackend final : public InitializableAudioBackend {
   public:
    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander,
                                       metalio_claw4::I2cExecutor& i2c_executor) override {
        (void)io_expander;
        (void)i2c_executor;
        return ESP_OK;
    }
    void SetMasterVolumePercent(uint8_t percent) override { (void)percent; }

    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t& tone) override {
        (void)tone;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t StartPcm(const device::PcmSource&, uint32_t, uint16_t,
                                   device::PcmStreamHandle& handle_out) override {
        handle_out = 0U;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t PausePcm(device::PcmStreamHandle) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t ResumePcm(device::PcmStreamHandle) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t SetPcmVolume(device::PcmStreamHandle, uint16_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t StopPcm(device::PcmStreamHandle) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    void BindPcmCompletionSink(device::PcmCompletionSink, void*) override {}
    void UnbindPcmCompletionSink(void*) override {}

    [[nodiscard]] int32_t StopAll() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t SuspendAll() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t ResumeAll() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
};

}  // namespace

InitializableAudioBackend& ConfiguredAudioBackend() {
    static NullAudioBackend backend;
    return backend;
}

}  // namespace micropixel::platform
