#include "platform/audio_backend.hpp"

namespace micropixel::platform {
namespace {

class NullAudioBackend final : public InitializableAudioBackend {
   public:
    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander) override {
        (void)io_expander;
        return ESP_OK;
    }

    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t& tone) override {
        (void)tone;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t StopAll() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
};

}  // namespace

InitializableAudioBackend& ConfiguredAudioBackend() {
    static NullAudioBackend backend;
    return backend;
}

}  // namespace micropixel::platform
