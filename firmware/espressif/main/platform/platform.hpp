#ifndef MICROPIXEL_PLATFORM_PLATFORM_HPP
#define MICROPIXEL_PLATFORM_PLATFORM_HPP

#include <array>
#include <cstdint>

#include "device/contracts/audio.hpp"
#include "device/contracts/battery.hpp"
#include "device/contracts/board_info.hpp"
#include "device/contracts/graphics.hpp"
#include "device/contracts/input.hpp"
#include "device/contracts/local_control.hpp"
#include "device/contracts/power.hpp"
#include "device/contracts/random.hpp"
#include "device/contracts/wifi.hpp"
#include "device/device_registry.hpp"
#include "esp_err.h"
#include "host/ui/system_ui.hpp"
#include "platform/audio/audio_engine.hpp"

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::platform::audio {
class AudioPowerController;
class AudioOutputPeripheral;
}  // namespace micropixel::platform::audio

namespace micropixel::platform {

// Fully normalized services consumed by FirmwareApp. Missing board features
// are filled with shared unavailable implementations during registration.
struct PlatformServices final {
    device::Graphics* graphics{};
    device::Input* input{};
    device::Audio* audio{};
    device::Battery* battery{};
    device::Random* random{};
    device::Wifi* wifi{};
    device::Power* power{};
    device::LocalControl* local_control{};
    device::DeviceCatalog* devices{};
    device::Sensors* sensors{};
    device::Gpio* gpio{};
    device::Haptics* haptics{};
    device::BoardInfo board_info{};
    host_ui::SystemUi* system_ui{};

    [[nodiscard]] bool Complete() const;
};

// Progressive board declaration. A board starts with identity, registers only
// hardware it has brought up, and may add optional peripherals over time.
class BoardRegistration final {
   public:
    explicit BoardRegistration(device::BoardInfo board_info) : board_info_(board_info) {}

    void SetGraphics(device::Graphics& graphics) { graphics_ = &graphics; }
    void SetInput(device::Input& input) { input_ = &input; }
    void SetAudio(device::Audio& audio) { audio_ = &audio; }
    void SetAudioOutput(audio::AudioOutputPeripheral& output, uint32_t sample_rate,
                        audio::AudioPowerController* power_controller = nullptr);
    void SetBattery(device::Battery& battery) { battery_ = &battery; }
    void SetWifi(device::Wifi& wifi) { wifi_ = &wifi; }
    void SetPower(device::Power& power) { power_ = &power; }
    void SetLocalControl(device::LocalControl& local_control) { local_control_ = &local_control; }
    void SetSystemUi(host_ui::SystemUi& system_ui) { system_ui_ = &system_ui; }
    [[nodiscard]] bool AddSensor(device::SensorPeripheral& peripheral, device::PeripheralChannelId channel,
                                 const char* name);
    [[nodiscard]] bool AddGpio(device::GpioPeripheral& peripheral, device::PeripheralChannelId channel,
                               const char* name);
    [[nodiscard]] bool AddHaptics(device::HapticsPeripheral& peripheral, device::PeripheralChannelId channel,
                                  const char* name);

   private:
    friend class Platform;
    template <typename PeripheralType>
    struct Peripheral final {
        PeripheralType* peripheral{};
        device::PeripheralChannelId channel{};
        char name[MICROPIXEL_DEVICE_NAME_MAX_BYTES + 1U]{};
    };

    device::BoardInfo board_info_{};
    device::Graphics* graphics_{};
    device::Input* input_{};
    device::Audio* audio_{};
    audio::AudioOutputPeripheral* audio_output_{};
    audio::AudioPowerController* audio_power_controller_{};
    uint32_t audio_sample_rate_{};
    device::Battery* battery_{};
    device::Wifi* wifi_{};
    device::Power* power_{};
    device::LocalControl* local_control_{};
    host_ui::SystemUi* system_ui_{};
    std::array<Peripheral<device::SensorPeripheral>, 8U> sensors_{};
    std::array<Peripheral<device::GpioPeripheral>, 32U> gpio_{};
    std::array<Peripheral<device::HapticsPeripheral>, 4U> haptics_{};
    uint32_t sensor_count_{};
    uint32_t gpio_count_{};
    uint32_t haptics_count_{};
};

class BoardContext;

// One concrete PCB/product composition selected by CMake. A Board owns only
// wiring, initialization order and its Peripheral, Controller and Presentation
// implementations.
class Board {
   public:
    virtual ~Board() = default;
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize(BoardContext& context) = 0;
    virtual void BindBackgroundExecutor(work::BackgroundExecutor& executor) = 0;

   protected:
    Board() = default;
};

class Platform;

// Narrow Board-to-Platform construction API. It exposes shared engines needed
// during bring-up and accepts exactly one completed BoardRegistration.
class BoardContext final {
   public:
    [[nodiscard]] audio::AudioEngine& AudioEngine();
    [[nodiscard]] bool Publish(const BoardRegistration& registration);

   private:
    friend class Platform;
    explicit BoardContext(Platform& platform) : platform_(platform) {}

    Platform& platform_;
};

// Generic assembly framework shared by every Board. Platform owns public
// Device identity, common engines, unavailable defaults and normalized
// services; it does not know concrete pins, buses or drivers.
class Platform final {
   public:
    explicit Platform(Board& board) : board_(board), board_context_(*this) {}
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] esp_err_t Initialize();
    void BindBackgroundExecutor(work::BackgroundExecutor& executor);
    [[nodiscard]] bool Ready() const { return services_.Complete(); }
    [[nodiscard]] const PlatformServices& Services() const { return services_; }

   private:
    friend class BoardContext;
    [[nodiscard]] bool Publish(const BoardRegistration& registration);
    [[nodiscard]] audio::AudioEngine& AudioEngine() { return audio_engine_; }

    Board& board_;
    BoardContext board_context_;
    PlatformServices services_{};
    device::DeviceRegistry device_registry_{};
    audio::AudioEngine audio_engine_{};
    bool initializing_{};
};

[[nodiscard]] Board& ConfiguredBoard();
[[nodiscard]] Platform& ConfiguredPlatform();

}  // namespace micropixel::platform

#endif
