#include "device/device_registry.hpp"

#include <cstring>

namespace micropixel::device {
namespace {

constexpr micropixel_device_id_t kRegistryDeviceIdBase = 0x01000000U;

}  // namespace

void DeviceRegistry::Reset() {
    for (uint32_t index = 0U; index < count_; ++index) {
        if (entries_[index].route_kind == RouteKind::kHaptics) {
            static_cast<HapticsPeripheral*>(entries_[index].peripheral)->SetCompletionSink(nullptr, nullptr);
        }
    }
    entries_ = {};
    count_ = 0U;
    ++generation_;
    if (generation_ == 0U) {
        generation_ = 1U;
    }
    haptic_completion_sink_ = nullptr;
    haptic_completion_context_ = nullptr;
}

bool DeviceRegistry::RegisterDisplay(const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_DISPLAY, MICROPIXEL_DEVICE_CAP_WRITE, name);
}

bool DeviceRegistry::RegisterTouch(const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_TOUCH, MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_EVENTS, name);
}

bool DeviceRegistry::RegisterAudioOutput(const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_AUDIO_OUTPUT, MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, name);
}

bool DeviceRegistry::RegisterPower(const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_POWER, MICROPIXEL_DEVICE_CAP_READ, name);
}

bool DeviceRegistry::RegisterSensor(SensorPeripheral& peripheral, PeripheralChannelId channel, const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_SENSOR, MICROPIXEL_DEVICE_CAP_READ, name, RouteKind::kSensor, &peripheral,
               channel);
}

bool DeviceRegistry::RegisterGpio(GpioPeripheral& peripheral, PeripheralChannelId channel, const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_GPIO_LINE,
               MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, name,
               RouteKind::kGpio, &peripheral, channel);
}

bool DeviceRegistry::RegisterHaptics(HapticsPeripheral& peripheral, PeripheralChannelId channel, const char* name) {
    return Add(MICROPIXEL_DEVICE_KIND_HAPTICS, MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, name,
               RouteKind::kHaptics, &peripheral, channel);
}

bool DeviceRegistry::Add(uint16_t kind, uint64_t capabilities, const char* name, RouteKind route_kind, void* peripheral,
                         PeripheralChannelId channel) {
    if (count_ >= entries_.size() || kind == MICROPIXEL_DEVICE_KIND_ANY || name == nullptr ||
        (route_kind != RouteKind::kNone && peripheral == nullptr)) {
        return false;
    }
    for (uint32_t index = 0U; index < count_; ++index) {
        if (entries_[index].route_kind == route_kind && entries_[index].peripheral == peripheral &&
            entries_[index].channel == channel && route_kind != RouteKind::kNone) {
            return false;
        }
    }
    Entry& entry = entries_[count_];
    entry.device = kRegistryDeviceIdBase | (count_ + 1U);
    entry.kind = kind;
    entry.capabilities = capabilities;
    entry.route_kind = route_kind;
    entry.peripheral = peripheral;
    entry.channel = channel;
    size_t length = 0U;
    while (length < MICROPIXEL_DEVICE_NAME_MAX_BYTES && name[length] != '\0') {
        ++length;
    }
    std::memcpy(entry.name, name, length);
    entry.name[length] = '\0';
    entry.name_length = static_cast<uint16_t>(length);
    ++count_;
    return true;
}

int32_t DeviceRegistry::GetByIndex(uint32_t index, micropixel_device_info_t& info_out) const {
    if (index >= count_) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    const Entry& entry = entries_[index];
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.kind = entry.kind;
    info_out.device = entry.device;
    info_out.parent = entry.parent;
    info_out.capabilities = entry.capabilities;
    std::memcpy(info_out.name, entry.name, static_cast<size_t>(entry.name_length) + 1U);
    info_out.name_length = entry.name_length;
    return MICROPIXEL_STATUS_OK;
}

int32_t DeviceRegistry::GetById(micropixel_device_id_t device, micropixel_device_info_t& info_out) const {
    for (uint32_t index = 0U; index < count_; ++index) {
        if (entries_[index].device == device) {
            return GetByIndex(index, info_out);
        }
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
}

DeviceRegistry::Entry* DeviceRegistry::Find(micropixel_device_id_t device) {
    for (uint32_t index = 0U; index < count_; ++index) {
        if (entries_[index].device == device) {
            return &entries_[index];
        }
    }
    return nullptr;
}

const DeviceRegistry::Entry* DeviceRegistry::Find(micropixel_device_id_t device) const {
    return const_cast<DeviceRegistry*>(this)->Find(device);
}

DeviceRegistry::Entry* DeviceRegistry::Find(GpioPeripheral& peripheral, PeripheralChannelId channel) {
    for (uint32_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.route_kind == RouteKind::kGpio && entry.peripheral == &peripheral && entry.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

DeviceRegistry::Entry* DeviceRegistry::Find(HapticsPeripheral& peripheral, PeripheralChannelId channel) {
    for (uint32_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.route_kind == RouteKind::kHaptics && entry.peripheral == &peripheral && entry.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

int32_t DeviceRegistry::GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const {
    const Entry* entry = Find(device);
    if (entry == nullptr || entry->route_kind != RouteKind::kSensor) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    const int32_t status = static_cast<SensorPeripheral*>(entry->peripheral)->GetInfo(entry->channel, info_out);
    if (status == MICROPIXEL_STATUS_OK) {
        info_out.device = device;
    }
    return status;
}

int32_t DeviceRegistry::Start(micropixel_device_id_t device, uint32_t interval_us) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kSensor
               ? static_cast<SensorPeripheral*>(entry->peripheral)->Start(entry->channel, interval_us)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

int32_t DeviceRegistry::Read(micropixel_device_id_t device, SensorValues& values_out) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kSensor
               ? static_cast<SensorPeripheral*>(entry->peripheral)->Read(entry->channel, values_out)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

void DeviceRegistry::StopSampling(micropixel_device_id_t device) {
    Entry* entry = Find(device);
    if (entry != nullptr && entry->route_kind == RouteKind::kSensor) {
        static_cast<SensorPeripheral*>(entry->peripheral)->Stop(entry->channel);
    }
}

int32_t DeviceRegistry::GetInfo(micropixel_device_id_t device, micropixel_gpio_info_t& info_out) const {
    const Entry* entry = Find(device);
    if (entry == nullptr || entry->route_kind != RouteKind::kGpio) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    const int32_t status = static_cast<GpioPeripheral*>(entry->peripheral)->GetInfo(entry->channel, info_out);
    if (status == MICROPIXEL_STATUS_OK) {
        info_out.device = device;
    }
    return status;
}

int32_t DeviceRegistry::Open(micropixel_device_id_t device, uint16_t mode, uint16_t pull, uint16_t edge,
                             uint32_t initial_value, uint32_t pwm_frequency_hz, GpioEdgeSink edge_sink,
                             void* edge_context) {
    Entry* entry = Find(device);
    if (entry == nullptr || entry->route_kind != RouteKind::kGpio) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    entry->gpio_edge_sink = edge_sink;
    entry->gpio_edge_context = edge_sink == nullptr ? nullptr : edge_context;
    const int32_t status =
        static_cast<GpioPeripheral*>(entry->peripheral)
            ->Open(entry->channel, mode, pull, edge, initial_value, pwm_frequency_hz, OnGpioEdge, this);
    if (status != MICROPIXEL_STATUS_OK) {
        entry->gpio_edge_sink = nullptr;
        entry->gpio_edge_context = nullptr;
    }
    return status;
}

int32_t DeviceRegistry::Read(micropixel_device_id_t device, bool& value_out) const {
    const Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kGpio
               ? static_cast<GpioPeripheral*>(entry->peripheral)->Read(entry->channel, value_out)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

int32_t DeviceRegistry::Write(micropixel_device_id_t device, bool value) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kGpio
               ? static_cast<GpioPeripheral*>(entry->peripheral)->Write(entry->channel, value)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

int32_t DeviceRegistry::SetPwmDuty(micropixel_device_id_t device, uint16_t duty_per_mille) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kGpio
               ? static_cast<GpioPeripheral*>(entry->peripheral)->SetPwmDuty(entry->channel, duty_per_mille)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

void DeviceRegistry::SuspendEvents() {
    for (uint32_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.route_kind != RouteKind::kGpio) {
            continue;
        }
        bool seen = false;
        for (uint32_t prior = 0U; prior < index; ++prior) {
            seen = seen ||
                   (entries_[prior].route_kind == RouteKind::kGpio && entries_[prior].peripheral == entry.peripheral);
        }
        if (!seen) {
            static_cast<GpioPeripheral*>(entry.peripheral)->SuspendEvents();
        }
    }
}

int32_t DeviceRegistry::ResumeEvents() {
    for (uint32_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.route_kind != RouteKind::kGpio) {
            continue;
        }
        bool seen = false;
        for (uint32_t prior = 0U; prior < index; ++prior) {
            seen = seen ||
                   (entries_[prior].route_kind == RouteKind::kGpio && entries_[prior].peripheral == entry.peripheral);
        }
        if (!seen) {
            const int32_t status = static_cast<GpioPeripheral*>(entry.peripheral)->ResumeEvents();
            if (status != MICROPIXEL_STATUS_OK) {
                return status;
            }
        }
    }
    return MICROPIXEL_STATUS_OK;
}

void DeviceRegistry::Close(micropixel_device_id_t device) {
    Entry* entry = Find(device);
    if (entry != nullptr && entry->route_kind == RouteKind::kGpio) {
        static_cast<GpioPeripheral*>(entry->peripheral)->Close(entry->channel);
        entry->gpio_edge_sink = nullptr;
        entry->gpio_edge_context = nullptr;
    }
}

void DeviceRegistry::OnGpioEdge(void* context, GpioPeripheral& peripheral, PeripheralChannelId channel, bool value,
                                uint64_t timestamp_us) {
    auto& registry = *static_cast<DeviceRegistry*>(context);
    Entry* entry = registry.Find(peripheral, channel);
    if (entry != nullptr && entry->gpio_edge_sink != nullptr) {
        entry->gpio_edge_sink(entry->gpio_edge_context, entry->device, value, timestamp_us);
    }
}

int32_t DeviceRegistry::GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const {
    const Entry* entry = Find(device);
    if (entry == nullptr || entry->route_kind != RouteKind::kHaptics) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    const int32_t status = static_cast<HapticsPeripheral*>(entry->peripheral)->GetInfo(entry->channel, info_out);
    if (status == MICROPIXEL_STATUS_OK) {
        info_out.device = device;
    }
    return status;
}

int32_t DeviceRegistry::Play(micropixel_device_id_t device, uint16_t strength_per_mille, uint32_t duration_ms) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kHaptics
               ? static_cast<HapticsPeripheral*>(entry->peripheral)
                     ->Play(entry->channel, strength_per_mille, duration_ms)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

int32_t DeviceRegistry::Stop(micropixel_device_id_t device) {
    Entry* entry = Find(device);
    return entry != nullptr && entry->route_kind == RouteKind::kHaptics
               ? static_cast<HapticsPeripheral*>(entry->peripheral)->Stop(entry->channel)
               : static_cast<int32_t>(MICROPIXEL_STATUS_NOT_FOUND);
}

void DeviceRegistry::SetCompletionSink(HapticCompletionSink sink, void* context) {
    haptic_completion_sink_ = sink;
    haptic_completion_context_ = sink == nullptr ? nullptr : context;
    for (uint32_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.route_kind != RouteKind::kHaptics) {
            continue;
        }
        bool seen = false;
        for (uint32_t prior = 0U; prior < index; ++prior) {
            seen = seen || (entries_[prior].route_kind == RouteKind::kHaptics &&
                            entries_[prior].peripheral == entry.peripheral);
        }
        if (!seen) {
            static_cast<HapticsPeripheral*>(entry.peripheral)->SetCompletionSink(OnHapticsFinished, this);
        }
    }
}

void DeviceRegistry::OnHapticsFinished(void* context, HapticsPeripheral& peripheral, PeripheralChannelId channel,
                                       uint64_t timestamp_us) {
    auto& registry = *static_cast<DeviceRegistry*>(context);
    Entry* entry = registry.Find(peripheral, channel);
    if (entry != nullptr && registry.haptic_completion_sink_ != nullptr) {
        registry.haptic_completion_sink_(registry.haptic_completion_context_, entry->device, timestamp_us);
    }
}

}  // namespace micropixel::device
