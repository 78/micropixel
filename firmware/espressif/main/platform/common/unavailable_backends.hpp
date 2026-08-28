#pragma once

#include <cstring>

#include "device/audio.hpp"
#include "device/battery.hpp"
#include "device/graphics.hpp"
#include "device/hardware_info.hpp"
#include "device/input.hpp"
#include "device/wifi.hpp"
#include "host_ui/system_ui.hpp"
#include "platform/platform.hpp"

namespace micropixel::platform::common {

class UnavailableGraphicsBackend final : public device::GraphicsBackend {
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

    [[nodiscard]] int32_t LoadFont(const device::FontResourceView&, micropixel_font_info_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t) override { return MICROPIXEL_STATUS_UNSUPPORTED; }

    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t, const char*, uint32_t,
                                      micropixel_text_metrics_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
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

class UnavailableAudioBackend final : public device::AudioBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t&) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
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

class UnavailableInputBackend final : public device::InputBackend {
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

class UnavailableBatteryBackend final : public device::BatteryBackend {
   public:
    [[nodiscard]] device::BatterySnapshot Snapshot() override { return {}; }
    void SetStateChangeSink(device::BatteryStateChangeSink, void*) override {}
};

class UnavailableWifiBackend final : public device::WifiBackend {
   public:
    [[nodiscard]] std::expected<void, device::WifiError> Initialize() override {
        return std::unexpected(device::WifiError::kUnavailable);
    }
    [[nodiscard]] device::WifiSnapshot Snapshot() const override { return {}; }
    void SetStateChangeSink(device::WifiStateChangeSink, void*) override {}
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

// Partial board bring-up backends may derive from this class and override only
// the views they can render without weakening the SystemUiBackend contract.
class UnavailableSystemUiBackend : public host_ui::SystemUiBackend {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return {};
    }
    void UpdateHallStatusBar(const host_ui::HallStatusBarModel& model) override { (void)model; }
    void LeaveHall() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override { return {}; }
    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override {
        (void)action_sink;
        (void)action_context;
    }
    void StopWatchingGuestActions(void* action_context) override { (void)action_context; }
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        uint32_t hall_app_index, uint64_t trigger_timestamp_us) override {
        (void)hall_app_index;
        (void)trigger_timestamp_us;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void ReleaseGuestSnapshot() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void UpdateSystemMenu(const host_ui::SystemMenuModel& model) override { (void)model; }
    void LeaveSystemMenu() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void LeaveSystemInformation() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowPowerManagement(const host_ui::PowerManagementModel&,
                                                                                  host_ui::SystemUiActionSink,
                                                                                  void*) override {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void UpdatePowerManagement(const host_ui::PowerManagementModel&) override {}
    void LeavePowerManagement() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowRemoteControl(
        const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void UpdateRemoteControl(const host_ui::RemoteControlModel& model) override { (void)model; }
    void LeaveRemoteControl() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagement(
        const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void LeaveAppManagement() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) override {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void UpdateWifiSettings(const host_ui::WifiSettingsModel& model) override { (void)model; }
    void LeaveWifiSettings() override {}
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              uint64_t trigger_timestamp_us,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) override {
        (void)model;
        (void)trigger_timestamp_us;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void UpdateStatusLayer(const host_ui::StatusLayerModel& model) override { (void)model; }
    void LeaveStatusLayer(uint64_t trigger_timestamp_us) override { (void)trigger_timestamp_us; }
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) override {
        (void)enabled;
        (void)cpu_percent;
    }
    void ApplyBrightness(uint8_t percent) override { (void)percent; }
    void ApplyVolume(uint8_t percent) override { (void)percent; }
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowShutdown() override {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
};

class UnavailableDeviceCatalogBackend final : public device::DeviceCatalogBackend {
   public:
    [[nodiscard]] uint32_t Generation() const override { return 1U; }
    [[nodiscard]] uint32_t Count() const override { return 0U; }
    [[nodiscard]] int32_t GetByIndex(uint32_t, micropixel_device_info_t&) const override {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    [[nodiscard]] int32_t GetById(micropixel_device_id_t, micropixel_device_info_t&) const override {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
};

class UnavailableSensorBackend final : public device::SensorBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t, micropixel_sensor_info_t&) const override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t Start(micropixel_device_id_t, uint32_t) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t Read(micropixel_device_id_t, device::SensorValues&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    void Stop(micropixel_device_id_t) override {}
};

class UnavailableGpioBackend final : public device::GpioBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t, micropixel_gpio_info_t&) const override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t Open(micropixel_device_id_t, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t,
                               device::GpioEdgeSink, void*) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t Read(micropixel_device_id_t, bool&) const override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t Write(micropixel_device_id_t, bool) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t SetPwmDuty(micropixel_device_id_t, uint16_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    void SuspendEvents() override {}
    [[nodiscard]] int32_t ResumeEvents() override { return MICROPIXEL_STATUS_OK; }
    void Close(micropixel_device_id_t) override {}
};

class UnavailableHapticsBackend final : public device::HapticsBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t, micropixel_haptics_info_t&) const override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t Play(micropixel_device_id_t, uint16_t, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t Stop(micropixel_device_id_t) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    void SetCompletionSink(device::HapticCompletionSink, void*) override {}
};

class UnavailableHardwareInfoBackend : public device::HardwareInfoBackend {
   public:
    [[nodiscard]] device::HardwareInfo Snapshot() const override { return {}; }
};

}  // namespace micropixel::platform::common
