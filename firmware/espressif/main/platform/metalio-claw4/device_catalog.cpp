#include "platform/metalio-claw4/device_catalog.hpp"

#include <cstdio>
#include <cstring>

#include "platform/metalio-claw4/peripheral_ids.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr uint32_t kFixedDeviceCount = 5U;

void BuildInfo(micropixel_device_info_t& info_out, micropixel_device_id_t id, uint16_t kind, uint64_t capabilities,
               const char* name, micropixel_device_id_t parent = 0U) {
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.kind = kind;
    info_out.device = id;
    info_out.parent = parent;
    info_out.capabilities = capabilities;
    size_t length = 0U;
    while (length < MICROPIXEL_DEVICE_NAME_MAX_BYTES && name[length] != '\0') {
        ++length;
    }
    std::memcpy(info_out.name, name, length);
    info_out.name[length] = '\0';
    info_out.name_length = static_cast<uint16_t>(length);
}

void BuildGpioInfo(micropixel_device_info_t& info_out, uint16_t line) {
    char name[16]{};
    (void)std::snprintf(name, sizeof(name), "GPIO %u", static_cast<unsigned>(line));
    BuildInfo(info_out, peripheral_ids::Gpio(line), MICROPIXEL_DEVICE_KIND_GPIO_LINE,
              MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, name);
}

}  // namespace

void DeviceCatalog::Initialize(bool acceleration_available, bool magnetic_field_available) {
    acceleration_available_ = acceleration_available;
    magnetic_field_available_ = magnetic_field_available;
    count_ = kFixedDeviceCount + peripheral_ids::kApplicationGpioLines.size() +
             static_cast<uint32_t>(acceleration_available_) + static_cast<uint32_t>(magnetic_field_available_);
}

int32_t DeviceCatalog::GetByIndex(uint32_t index, micropixel_device_info_t& info_out) const {
    if (index >= count_) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (index == 0U) {
        BuildInfo(info_out, peripheral_ids::kDisplay, MICROPIXEL_DEVICE_KIND_DISPLAY, MICROPIXEL_DEVICE_CAP_WRITE,
                  "Built-in display");
        return MICROPIXEL_STATUS_OK;
    }
    if (index == 1U) {
        BuildInfo(info_out, peripheral_ids::kTouch, MICROPIXEL_DEVICE_KIND_TOUCH,
                  MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_EVENTS, "Built-in touchscreen");
        return MICROPIXEL_STATUS_OK;
    }
    if (index == 2U) {
        BuildInfo(info_out, peripheral_ids::kAudioOutput, MICROPIXEL_DEVICE_KIND_AUDIO_OUTPUT,
                  MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, "Built-in speaker");
        return MICROPIXEL_STATUS_OK;
    }
    index -= 3U;
    if (acceleration_available_) {
        if (index == 0U) {
            BuildInfo(info_out, peripheral_ids::kAcceleration, MICROPIXEL_DEVICE_KIND_SENSOR,
                      MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_EVENTS, "Built-in accelerometer");
            return MICROPIXEL_STATUS_OK;
        }
        --index;
    }
    if (magnetic_field_available_) {
        if (index == 0U) {
            BuildInfo(info_out, peripheral_ids::kMagneticField, MICROPIXEL_DEVICE_KIND_SENSOR,
                      MICROPIXEL_DEVICE_CAP_READ | MICROPIXEL_DEVICE_CAP_EVENTS, "Built-in magnetometer");
            return MICROPIXEL_STATUS_OK;
        }
        --index;
    }
    if (index < peripheral_ids::kApplicationGpioLines.size()) {
        BuildGpioInfo(info_out, peripheral_ids::kApplicationGpioLines[index]);
        return MICROPIXEL_STATUS_OK;
    }
    index -= peripheral_ids::kApplicationGpioLines.size();
    if (index == 0U) {
        BuildInfo(info_out, peripheral_ids::kHaptics, MICROPIXEL_DEVICE_KIND_HAPTICS,
                  MICROPIXEL_DEVICE_CAP_WRITE | MICROPIXEL_DEVICE_CAP_EVENTS, "Built-in vibration motor");
        return MICROPIXEL_STATUS_OK;
    }
    BuildInfo(info_out, peripheral_ids::kPower, MICROPIXEL_DEVICE_KIND_POWER, MICROPIXEL_DEVICE_CAP_READ,
              "System power");
    return MICROPIXEL_STATUS_OK;
}

int32_t DeviceCatalog::GetById(micropixel_device_id_t device, micropixel_device_info_t& info_out) const {
    for (uint32_t index = 0U; index < count_; ++index) {
        micropixel_device_info_t candidate{};
        if (GetByIndex(index, candidate) == MICROPIXEL_STATUS_OK && candidate.device == device) {
            info_out = candidate;
            return MICROPIXEL_STATUS_OK;
        }
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
}

}  // namespace micropixel::platform::metalio_claw4
