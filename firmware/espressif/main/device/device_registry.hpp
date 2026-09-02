#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "device/contracts/devices.hpp"
#include "device/contracts/gpio.hpp"
#include "device/contracts/haptics.hpp"
#include "device/contracts/sensors.hpp"
#include "sdkconfig.h"

namespace micropixel::device {

// Host-owned registry joining public DeviceId values to board-local peripheral
// channels. Boards contribute capabilities; they never allocate ABI IDs or
// construct the Guest-visible catalog.
class DeviceRegistry final : public DeviceCatalog, public Sensors, public Gpio, public Haptics {
   public:
    static constexpr uint32_t kMaximumDeviceCount = MICROPIXEL_MAX_DEVICES;

    [[nodiscard]] bool InitializeStorage();
    void Reset();
    [[nodiscard]] bool RegisterDisplay(const char* name = "Built-in display");
    [[nodiscard]] bool RegisterTouch(const char* name = "Built-in touchscreen");
    [[nodiscard]] bool RegisterAudioOutput(const char* name = "Built-in speaker");
    [[nodiscard]] bool RegisterPower(const char* name = "System power");
    [[nodiscard]] bool RegisterSensor(SensorPeripheral& peripheral, PeripheralChannelId channel, const char* name);
    [[nodiscard]] bool RegisterGpio(GpioPeripheral& peripheral, PeripheralChannelId channel, const char* name);
    [[nodiscard]] bool RegisterHaptics(HapticsPeripheral& peripheral, PeripheralChannelId channel, const char* name);

    [[nodiscard]] uint32_t Generation() const override { return generation_; }
    [[nodiscard]] uint32_t Count() const override { return count_; }
    [[nodiscard]] int32_t GetByIndex(uint32_t index, micropixel_device_info_t& info_out) const override;
    [[nodiscard]] int32_t GetById(micropixel_device_id_t device, micropixel_device_info_t& info_out) const override;

    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(micropixel_device_id_t device, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(micropixel_device_id_t device, SensorValues& values_out) override;
    void StopSampling(micropixel_device_id_t device) override;

    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_gpio_info_t& info_out) const override;
    [[nodiscard]] int32_t Open(micropixel_device_id_t device, uint16_t mode, uint16_t pull, uint16_t edge,
                               uint32_t initial_value, uint32_t pwm_frequency_hz, GpioEdgeSink edge_sink,
                               void* edge_context) override;
    [[nodiscard]] int32_t Read(micropixel_device_id_t device, bool& value_out) const override;
    [[nodiscard]] int32_t Write(micropixel_device_id_t device, bool value) override;
    [[nodiscard]] int32_t SetPwmDuty(micropixel_device_id_t device, uint16_t duty_per_mille) override;
    void SuspendEvents() override;
    [[nodiscard]] int32_t ResumeEvents() override;
    void Close(micropixel_device_id_t device) override;

    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const override;
    [[nodiscard]] int32_t Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                               uint32_t duration_ms) override;
    [[nodiscard]] int32_t Stop(micropixel_device_id_t device) override;
    void SetCompletionSink(HapticCompletionSink sink, void* context) override;

   private:
    enum class RouteKind : uint8_t { kNone, kSensor, kGpio, kHaptics };

    struct Entry final {
        micropixel_device_id_t device{};
        uint16_t kind{};
        uint64_t capabilities{};
        char name[MICROPIXEL_DEVICE_NAME_MAX_BYTES + 1U]{};
        uint16_t name_length{};
        micropixel_device_id_t parent{};
        RouteKind route_kind{RouteKind::kNone};
        void* peripheral{};
        PeripheralChannelId channel{};
        GpioEdgeSink gpio_edge_sink{};
        void* gpio_edge_context{};
    };

#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    struct EntryDeleter final {
        void operator()(Entry* entries) const;
    };
#endif

    [[nodiscard]] bool Add(uint16_t kind, uint64_t capabilities, const char* name,
                           RouteKind route_kind = RouteKind::kNone, void* peripheral = nullptr,
                           PeripheralChannelId channel = 0U);
    [[nodiscard]] Entry* Find(micropixel_device_id_t device);
    [[nodiscard]] const Entry* Find(micropixel_device_id_t device) const;
    [[nodiscard]] Entry* Find(GpioPeripheral& peripheral, PeripheralChannelId channel);
    [[nodiscard]] Entry* Find(HapticsPeripheral& peripheral, PeripheralChannelId channel);
    static void OnGpioEdge(void* context, GpioPeripheral& peripheral, PeripheralChannelId channel, bool value,
                           uint64_t timestamp_us);
    static void OnHapticsFinished(void* context, HapticsPeripheral& peripheral, PeripheralChannelId channel,
                                  uint64_t timestamp_us);

#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    [[nodiscard]] Entry* Entries() { return entries_.get(); }
    [[nodiscard]] const Entry* Entries() const { return entries_.get(); }
    std::unique_ptr<Entry, EntryDeleter> entries_{};
#else
    [[nodiscard]] Entry* Entries() { return entries_.data(); }
    [[nodiscard]] const Entry* Entries() const { return entries_.data(); }
    std::array<Entry, kMaximumDeviceCount> entries_{};
#endif
    uint32_t count_{};
    uint32_t generation_{1U};
    HapticCompletionSink haptic_completion_sink_{};
    void* haptic_completion_context_{};
};

}  // namespace micropixel::device
