#pragma once
#include "device/device_services.hpp"
#include "runtime/bundle/app_requirements.h"

namespace micropixel::runtime {
inline micropixel_app_environment_t AppEnvironment(device::DeviceServices& devices) {
    micropixel_app_environment_t result{};
    result.core_abi = MICROPIXEL_ABI_VERSION;
    const auto graphics = devices.graphics().GetInfo();
    if (graphics) {
        micropixel_app_set_logical_display(&result, graphics->width, graphics->height);
    }
    const auto input = devices.input().GetInfo();
    if (input) {
        if (input->max_touch_points != 0U) result.capabilities |= 1U;
        if ((input->capabilities & MICROPIXEL_INPUT_CAP_KEY_EVENTS) != 0U) result.capabilities |= 2U;
    }
    for (uint16_t first = 0; first < 64;) {
        const auto page = devices.devices().List(MICROPIXEL_DEVICE_KIND_ANY, first);
        if (!page || page->count == 0U) break;
        for (uint16_t index = 0; index < page->count; ++index) {
            const auto info = devices.devices().GetInfo(page->devices[index]);
            if (!info) continue;
            if (info->kind == MICROPIXEL_DEVICE_KIND_AUDIO_OUTPUT) result.capabilities |= 4U;
            if (info->kind == MICROPIXEL_DEVICE_KIND_HAPTICS) result.capabilities |= 64U;
            if (info->kind == MICROPIXEL_DEVICE_KIND_GPIO_LINE) result.capabilities |= 128U;
            if (info->kind == MICROPIXEL_DEVICE_KIND_SENSOR) {
                const auto sensor = devices.sensors().GetInfo(page->devices[index]);
                if (sensor && sensor->kind >= MICROPIXEL_SENSOR_ACCELERATION &&
                    sensor->kind <= MICROPIXEL_SENSOR_MAGNETIC_FIELD) {
                    result.capabilities |= static_cast<uint8_t>(1U << (sensor->kind + 2U));
                }
            }
        }
        first = static_cast<uint16_t>(first + page->count);
        if (first >= page->total_count) break;
    }
    result.services[0] = (MICROPIXEL_SYSTEM_INTERFACE_MAJOR << 16U) | MICROPIXEL_SYSTEM_INTERFACE_MINOR;
    result.services[1] = (MICROPIXEL_INPUT_INTERFACE_MAJOR << 16U) | MICROPIXEL_INPUT_INTERFACE_MINOR;
    result.services[2] = (MICROPIXEL_GRAPHICS_INTERFACE_MAJOR << 16U) | MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    result.services[3] = (MICROPIXEL_AUDIO_INTERFACE_MAJOR << 16U) | MICROPIXEL_AUDIO_INTERFACE_MINOR;
    result.services[4] = 1U << 16U;
    result.services[5] = 1U << 16U;
    result.services[6] = (MICROPIXEL_RESOURCE_INTERFACE_MAJOR << 16U) | MICROPIXEL_RESOURCE_INTERFACE_MINOR;
    result.services[7] = (MICROPIXEL_DEVICES_INTERFACE_MAJOR << 16U) | MICROPIXEL_DEVICES_INTERFACE_MINOR;
    result.services[8] = (MICROPIXEL_SENSORS_INTERFACE_MAJOR << 16U) | MICROPIXEL_SENSORS_INTERFACE_MINOR;
    result.services[9] = (MICROPIXEL_GPIO_INTERFACE_MAJOR << 16U) | MICROPIXEL_GPIO_INTERFACE_MINOR;
    result.services[10] = (MICROPIXEL_HAPTICS_INTERFACE_MAJOR << 16U) | MICROPIXEL_HAPTICS_INTERFACE_MINOR;
    return result;
}
}  // namespace micropixel::runtime
