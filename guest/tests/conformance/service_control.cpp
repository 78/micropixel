#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(0xffffffffU, MICROPIXEL_INTERFACE_VERSION(1U, 0U), &service, sizeof(service)) !=
        MICROPIXEL_STATUS_NOT_FOUND) {
        return 120;
    }
    if (micropixel_service_open(MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_INTERFACE_VERSION(2U, 0U), &service,
                                sizeof(service)) != MICROPIXEL_STATUS_VERSION_MISMATCH) {
        return 126;
    }
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service) - 1U) != MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
        return 121;
    }
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service)) != MICROPIXEL_STATUS_OK ||
        service.size != sizeof(service) || service.service_id != MICROPIXEL_SERVICE_GRAPHICS ||
        service.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
        (service.flags & MICROPIXEL_SERVICE_FLAG_SUBMIT) == 0U || service.handle == 0U) {
        return 122;
    }

    uint8_t dummy = 0U;
    uint32_t response_size = 0U;
    micropixel_graphics_info_t graphics{};
    if (micropixel_service_call(service.handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, &dummy, 1U,
                                reinterpret_cast<uint8_t*>(&graphics), sizeof(graphics),
                                &response_size) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 123;
    }
    if (micropixel_service_call(service.handle, 0xffffffffU, &dummy, 0U, reinterpret_cast<uint8_t*>(&graphics),
                                sizeof(graphics), &response_size) != MICROPIXEL_STATUS_UNSUPPORTED) {
        return 124;
    }
    if (micropixel_service_call(service.handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, &dummy, 0U,
                                reinterpret_cast<uint8_t*>(&graphics), sizeof(graphics),
                                &response_size) != MICROPIXEL_STATUS_OK ||
        response_size != sizeof(graphics) || graphics.size != sizeof(graphics)) {
        return 125;
    }

    micropixel_service_info_t random_service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_RANDOM,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_RANDOM_INTERFACE_MAJOR, MICROPIXEL_RANDOM_INTERFACE_MINOR),
            &random_service, sizeof(random_service)) != MICROPIXEL_STATUS_OK ||
        random_service.service_id != MICROPIXEL_SERVICE_RANDOM || random_service.handle == 0U ||
        (random_service.flags & MICROPIXEL_SERVICE_FLAG_CALL) == 0U ||
        random_service.max_response_bytes < sizeof(micropixel_random_u32_response_t)) {
        return 127;
    }
    micropixel_random_u32_response_t random_value{};
    if (micropixel_service_call(random_service.handle, MICROPIXEL_RANDOM_METHOD_GET_U32, &dummy, 1U,
                                reinterpret_cast<uint8_t*>(&random_value), sizeof(random_value),
                                &response_size) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 128;
    }
    if (micropixel_service_call(random_service.handle, MICROPIXEL_RANDOM_METHOD_GET_U32, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&random_value), sizeof(random_value),
                                &response_size) != MICROPIXEL_STATUS_OK ||
        response_size != sizeof(random_value) || random_value.size != sizeof(random_value)) {
        return 129;
    }

    micropixel::Application app;
    (void)app.random().U32();
    app.log().Info("service_control: open/call routing, graphics submit, and hardware random ready");
    return 0;
}
