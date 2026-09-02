#include "platform/platform.hpp"

#include <cstring>
#include <expected>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/audio/audio_output_peripheral.hpp"
#include "platform/audio/audio_power_controller.hpp"
#include "platform/defaults/unavailable_services.hpp"
#include "platform/random/system_random.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_platform";

class UnavailablePower final : public device::Power {
   public:
    void SetPowerButtonSink(device::PowerButtonSink, void*) override {}
    void SetPowerOffButtonSink(device::PowerOffButtonSink, void*) override {}
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kUnavailable);
    }
    [[noreturn]] void PowerOff() override {
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
};

class UnavailableLocalControl final : public device::LocalControl {
   public:
    void Bind(device::LocalControlCommandSink, device::LocalControlResponseSource, void*) override {}
    void Unbind(void*) override {}
    void NotifyResponseReady() override {}
};

defaults::UnavailableGraphics& MissingGraphics() {
    static defaults::UnavailableGraphics value;
    return value;
}
defaults::UnavailableInput& MissingInput() {
    static defaults::UnavailableInput value;
    return value;
}
defaults::UnavailableAudio& MissingAudio() {
    static defaults::UnavailableAudio value;
    return value;
}
defaults::UnavailableBattery& MissingBattery() {
    static defaults::UnavailableBattery value;
    return value;
}
defaults::UnavailableWifi& MissingWifi() {
    static defaults::UnavailableWifi value;
    return value;
}
UnavailablePower& MissingPower() {
    static UnavailablePower value;
    return value;
}
UnavailableLocalControl& MissingLocalControl() {
    static UnavailableLocalControl value;
    return value;
}
defaults::UnavailableSystemUi& MissingSystemUi() {
    static defaults::UnavailableSystemUi value;
    return value;
}
}  // namespace

bool PlatformServices::Complete() const {
    return graphics != nullptr && input != nullptr && audio != nullptr && battery != nullptr && random != nullptr &&
           wifi != nullptr && power != nullptr && local_control != nullptr && devices != nullptr &&
           sensors != nullptr && gpio != nullptr && haptics != nullptr && board_info.board != nullptr &&
           system_ui != nullptr;
}

void BoardRegistration::SetAudioOutput(audio::AudioOutputPeripheral& output, uint32_t sample_rate,
                                       audio::AudioPowerController* power_controller) {
    audio_output_ = &output;
    audio_sample_rate_ = sample_rate;
    audio_power_controller_ = power_controller;
    audio_ = nullptr;
}

bool BoardRegistration::AddSensor(device::SensorPeripheral& peripheral, device::PeripheralChannelId channel,
                                  const char* name) {
    if (sensor_count_ >= sensors_.size() || name == nullptr) {
        return false;
    }
    auto& registration = sensors_[sensor_count_++];
    registration.peripheral = &peripheral;
    registration.channel = channel;
    std::strncpy(registration.name, name, MICROPIXEL_DEVICE_NAME_MAX_BYTES);
    return true;
}

bool BoardRegistration::AddGpio(device::GpioPeripheral& peripheral, device::PeripheralChannelId channel,
                                const char* name) {
    if (gpio_count_ >= gpio_.size() || name == nullptr) {
        return false;
    }
    auto& registration = gpio_[gpio_count_++];
    registration.peripheral = &peripheral;
    registration.channel = channel;
    std::strncpy(registration.name, name, MICROPIXEL_DEVICE_NAME_MAX_BYTES);
    return true;
}

bool BoardRegistration::AddHaptics(device::HapticsPeripheral& peripheral, device::PeripheralChannelId channel,
                                   const char* name) {
    if (haptics_count_ >= haptics_.size() || name == nullptr) {
        return false;
    }
    auto& registration = haptics_[haptics_count_++];
    registration.peripheral = &peripheral;
    registration.channel = channel;
    std::strncpy(registration.name, name, MICROPIXEL_DEVICE_NAME_MAX_BYTES);
    return true;
}

bool Platform::Publish(const BoardRegistration& registration) {
    if (!initializing_ || Ready() || registration.board_info_.board == nullptr) {
        return false;
    }
    device_registry_.Reset();
    bool valid = true;
    if (registration.graphics_ != nullptr) {
        valid = device_registry_.RegisterDisplay() && valid;
    }
    if (registration.input_ != nullptr) {
        valid = device_registry_.RegisterTouch() && valid;
    }

    device::Audio* audio = registration.audio_;
    if (registration.audio_output_ != nullptr) {
        const esp_err_t audio_status =
            registration.audio_sample_rate_ == 0U
                ? ESP_ERR_INVALID_ARG
                : audio_engine_.Initialize(*registration.audio_output_, registration.audio_sample_rate_,
                                           registration.audio_power_controller_);
        if (audio_status == ESP_OK) {
            audio = &audio_engine_;
        } else {
            ESP_LOGW(kTag, "%s audio capability unavailable for this boot: %s", registration.audio_output_->Name(),
                     esp_err_to_name(audio_status));
            registration.audio_output_->Shutdown();
            audio = nullptr;
        }
    }
    if (audio != nullptr) {
        valid = device_registry_.RegisterAudioOutput() && valid;
    }
    if (registration.battery_ != nullptr || registration.power_ != nullptr) {
        valid = device_registry_.RegisterPower() && valid;
    }
    for (uint32_t index = 0U; index < registration.sensor_count_; ++index) {
        const auto& channel = registration.sensors_[index];
        valid = device_registry_.RegisterSensor(*channel.peripheral, channel.channel, channel.name) && valid;
    }
    for (uint32_t index = 0U; index < registration.gpio_count_; ++index) {
        const auto& channel = registration.gpio_[index];
        valid = device_registry_.RegisterGpio(*channel.peripheral, channel.channel, channel.name) && valid;
    }
    for (uint32_t index = 0U; index < registration.haptics_count_; ++index) {
        const auto& channel = registration.haptics_[index];
        valid = device_registry_.RegisterHaptics(*channel.peripheral, channel.channel, channel.name) && valid;
    }
    if (!valid) {
        return false;
    }

    services_ = {
        .graphics = registration.graphics_ != nullptr ? registration.graphics_ : &MissingGraphics(),
        .input = registration.input_ != nullptr ? registration.input_ : &MissingInput(),
        .audio = audio != nullptr ? audio : &MissingAudio(),
        .battery = registration.battery_ != nullptr ? registration.battery_ : &MissingBattery(),
        .random = &random::SystemRandom(),
        .wifi = registration.wifi_ != nullptr ? registration.wifi_ : &MissingWifi(),
        .power = registration.power_ != nullptr ? registration.power_ : &MissingPower(),
        .local_control = registration.local_control_ != nullptr ? registration.local_control_ : &MissingLocalControl(),
        .devices = &device_registry_,
        .sensors = &device_registry_,
        .gpio = &device_registry_,
        .haptics = &device_registry_,
        .board_info = registration.board_info_,
        .system_ui = registration.system_ui_ != nullptr ? registration.system_ui_ : &MissingSystemUi(),
    };
    return services_.Complete();
}

audio::AudioEngine& BoardContext::AudioEngine() { return platform_.AudioEngine(); }

bool BoardContext::Publish(const BoardRegistration& registration) { return platform_.Publish(registration); }

esp_err_t Platform::Initialize() {
    if (initializing_ || Ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!device_registry_.InitializeStorage()) {
        ESP_LOGE(kTag, "device registry storage is unavailable");
        return ESP_ERR_NO_MEM;
    }
    initializing_ = true;
    const esp_err_t status = board_.Initialize(board_context_);
    initializing_ = false;
    if (status != ESP_OK) {
        return status;
    }
    return Ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void Platform::BindBackgroundExecutor(work::BackgroundExecutor& executor) { board_.BindBackgroundExecutor(executor); }

Platform& ConfiguredPlatform() {
    static Platform platform(ConfiguredBoard());
    return platform;
}

}  // namespace micropixel::platform
