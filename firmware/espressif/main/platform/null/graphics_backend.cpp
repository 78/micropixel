#include <cstring>

#include "device/graphics.hpp"
#include "device/input.hpp"
#include "device/wifi.hpp"
#include "host_ui/system_ui.hpp"
#include "platform/audio_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/platform.hpp"

namespace micropixel::platform {
namespace {

class NullGraphicsBackend final : public device::GraphicsBackend {
   public:
    [[nodiscard]] bool Available() const override { return false; }

    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t BeginFrame() override {
        if (graphics_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        graphics_frame_active_ = true;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length,
                                 const device::TextureAccess& textures) override {
        (void)bytes;
        (void)length;
        (void)textures;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t CommitFrame(const device::TextureAccess& textures) override {
        (void)textures;
        if (!graphics_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        graphics_frame_active_ = false;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t CancelFrame() override {
        if (!graphics_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        graphics_frame_active_ = false;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override {
        if (bitmap_update_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        bitmap_update_frame_active_ = true;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride) override {
        const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                             ? 3U
                                             : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U);
        if (bitmap.data == nullptr || pixels == nullptr || width == 0U || height == 0U || bytes_per_pixel == 0U ||
            (bitmap.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) == 0U || x + width > bitmap.width ||
            y + height > bitmap.height || stride != width * bytes_per_pixel) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        auto* destination = const_cast<uint8_t*>(bitmap.data) + y * bitmap.stride + x * bytes_per_pixel;
        for (uint32_t row = 0U; row < height; ++row) {
            std::memcpy(destination + row * bitmap.stride, pixels + row * stride, stride);
        }
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override {
        if (!bitmap_update_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        bitmap_update_frame_active_ = false;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap) override {
        (void)bitmap;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    void DismissLaunchBitmap() override {}
    void ReleaseGuestResources() override {
        bitmap_update_frame_active_ = false;
        graphics_frame_active_ = false;
    }

   private:
    bool bitmap_update_frame_active_{};
    bool graphics_frame_active_{};
};

class NullInputBackend final : public device::InputBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    void BindTouchSink(device::TouchSink sink, void* context) override {
        (void)sink;
        (void)context;
    }

    void UnbindTouchSink(void* context) override { (void)context; }
};

class NullWifiBackend final : public device::WifiBackend {
   public:
    [[nodiscard]] std::expected<void, device::WifiError> Initialize() override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] device::WifiSnapshot Snapshot() const override { return {}; }
    [[nodiscard]] std::expected<void, device::WifiError> SetEnabled(bool) override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] std::expected<void, device::WifiError> RequestScan() override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] std::expected<void, device::WifiError> ConnectSaved(std::string_view) override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] std::expected<void, device::WifiError> Connect(std::string_view, std::string_view) override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] std::expected<void, device::WifiError> Disconnect() override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] std::expected<void, device::WifiError> Forget(std::string_view) override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
};

class NullSystemUiBackend final : public host_ui::SystemUiBackend {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void UpdateHallWifi(const host_ui::HallWifiModel& model) override { (void)model; }
    void LeaveHall() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override { return {}; }
    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override {
        (void)action_sink;
        (void)action_context;
    }
    void StopWatchingGuestActions(void* action_context) override { (void)action_context; }
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame() override {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void ReleaseGuestSnapshot() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void LeaveSystemMenu() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void LeaveSystemInformation() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagement(
        const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void LeaveAppManagement() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void UpdateWifiSettings(const host_ui::WifiSettingsModel& model) override { (void)model; }
    void LeaveWifiSettings() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void UpdateStatusLayer(const host_ui::StatusLayerModel& model) override { (void)model; }
    void LeaveStatusLayer() override {}
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) override {
        (void)enabled;
        (void)cpu_percent;
    }
    void ApplyBrightness(uint8_t percent) override { (void)percent; }
    void ApplyVolume(uint8_t percent) override { (void)percent; }
};

class NullPlatform final : public Platform {
   public:
    [[nodiscard]] esp_err_t Initialize() override { return ESP_OK; }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return input_; }
    [[nodiscard]] device::AudioBackend& audio() override { return ConfiguredAudioBackend(); }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] device::WifiBackend& wifi() override { return wifi_; }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }

   private:
    NullGraphicsBackend graphics_{};
    NullInputBackend input_{};
    NullWifiBackend wifi_{};
    NullSystemUiBackend system_ui_{};
};

}  // namespace

Platform& ConfiguredPlatform() {
    static NullPlatform platform;
    return platform;
}

}  // namespace micropixel::platform
