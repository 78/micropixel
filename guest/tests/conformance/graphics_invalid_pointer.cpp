#include <stdint.h>

#include "abi/micropixel_abi.h"

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service)) != MICROPIXEL_STATUS_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    constexpr uintptr_t kBeyondGuestMemory = 0xfffffff0U;
    return micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                     reinterpret_cast<const uint8_t*>(kBeyondGuestMemory),
                                     sizeof(micropixel_graphics_scene_header_t));
}
