// Graphics 1.5 Direct Surface, negative path on the raw ABI with Host-owned
// buffers (the default; the Bundle needs no pinned memory): every malformed
// request must be refused with the documented status and never trap the Guest.
// Exit codes 50..79 name the failed step.
#include <stdint.h>

#include "abi/micropixel_abi.h"

namespace {

uint32_t g_service_handle;

int32_t Call(uint32_t method, const void* request, uint32_t request_size, void* response, uint32_t capacity) {
    uint32_t response_size = 0U;
    return micropixel_service_call(g_service_handle, method, static_cast<const uint8_t*>(request), request_size,
                                   static_cast<uint8_t*>(response), capacity, &response_size);
}

// Host buffers are named by index; pixels and length stay 0.
micropixel_surface_present_request_t ValidPresent(micropixel_surface_handle_t surface, uint32_t width,
                                                  uint32_t height) {
    micropixel_surface_present_request_t request{};
    request.size = sizeof(request);
    request.surface = surface;
    request.buffer_index = 0U;
    request.pitch = width * 2U;
    request.src_width = width;
    request.src_height = height;
    return request;
}

}  // namespace

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service)) != MICROPIXEL_STATUS_OK) {
        return 50;
    }
    g_service_handle = service.handle;

    micropixel_graphics_info_t info{};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U, &info, sizeof(info)) != MICROPIXEL_STATUS_OK ||
        info.size < sizeof(info)) {
        return 51;
    }
    if (info.native_pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) {
        return 52;
    }

    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = info.width;
    create.height = info.height;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = MICROPIXEL_SURFACE_MAX_BUFFERS + 1U;
    micropixel_surface_create_response_t created{};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 53;
    }
    create.buffer_count = 2U;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 54;
    }
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.width = info.width / 2U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 55;
    }
    create.width = info.width;
    // Unknown create flag bits.
    create.flags = 0x80000000U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 73;
    }
    // Guest buffers need a pinned linear memory, which this Bundle does not
    // declare: refused as unsupported, not trapped.
    create.flags = MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
        MICROPIXEL_STATUS_UNSUPPORTED) {
        return 74;
    }
    create.flags = 0U;

    // Presenting before any surface exists.
    micropixel_surface_present_request_t present = ValidPresent(1U, info.width, info.height);
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_NOT_FOUND) {
        return 56;
    }

    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
            MICROPIXEL_STATUS_OK ||
        created.size != sizeof(created) || created.surface == 0U ||
        created.native_pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) {
        return 57;
    }
    // Only one surface per Guest.
    micropixel_surface_create_response_t second{};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &second, sizeof(second)) !=
        MICROPIXEL_STATUS_RESOURCE_EXHAUSTED) {
        return 58;
    }
    // Scene submits are refused while the surface is alive.
    micropixel_graphics_scene_header_t scene{};
    scene.magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC;
    scene.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    scene.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    scene.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    scene.total_size = sizeof(scene);
    scene.generation = 1U;
    scene.revision = 1U;
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&scene), sizeof(scene)) == MICROPIXEL_STATUS_OK) {
        return 59;
    }

    // Wrong surface handle.
    present = ValidPresent(created.surface + 1U, info.width, info.height);
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_NOT_FOUND) {
        return 60;
    }
    // buffer_index beyond buffer_count.
    present = ValidPresent(created.surface, info.width, info.height);
    present.buffer_index = 2U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 61;
    }
    // A Host buffer may not be addressed: any pixels or length is refused.
    present = ValidPresent(created.surface, info.width, info.height);
    present.pixels = 0x40U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 62;
    }
    present = ValidPresent(created.surface, info.width, info.height);
    present.length = info.width * info.height * 2U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 63;
    }
    // Source size that is neither the panel nor an integer fraction of it.
    present = ValidPresent(created.surface, info.width, info.height);
    present.src_width = info.width - 1U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 64;
    }
    // Host buffers are panel-sized: a scaled present has nothing to scale.
    present = ValidPresent(created.surface, info.width / 2U, info.height / 2U);
    present.flags = MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 65;
    }
    // Unknown flag bits.
    present = ValidPresent(created.surface, info.width, info.height);
    present.flags = 0x80000000U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 66;
    }

    // A valid present is accepted once; presenting the same buffer while the
    // Host still holds it is a stale-state error, not a trap.
    present = ValidPresent(created.surface, info.width, info.height);
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_OK) {
        return 67;
    }
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
        MICROPIXEL_STATUS_STALE_STATE) {
        return 68;
    }
    // Nor may a raster list target the in-flight buffer; a free one is fine
    // when the Host has kernels (RECT needs no palette).
    if ((service.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) != 0U && info.raster_pool_bytes != 0U) {
        struct {
            micropixel_raster_header_t header;
            micropixel_raster_rect_t rect;
        } list{};
        list.header.magic = MICROPIXEL_GRAPHICS_RASTER_MAGIC;
        list.header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
        list.header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
        list.header.total_size = sizeof(list);
        list.header.target_buffer = 0U;
        list.header.target_width = static_cast<uint16_t>(info.width);
        list.header.target_height = static_cast<uint16_t>(info.height);
        list.header.target_pitch = static_cast<uint16_t>(info.width * 2U);
        list.header.record_count = 1U;
        list.rect.type = MICROPIXEL_RASTER_RECORD_RECT;
        list.rect.alpha = 255U;
        list.rect.width = 8U;
        list.rect.height = 8U;
        list.rect.color = 0xFFFFU;
        if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_RASTER,
                                      reinterpret_cast<const uint8_t*>(&list),
                                      sizeof(list)) != MICROPIXEL_STATUS_STALE_STATE) {
            return 75;
        }
        list.header.target_buffer = 1U;
        if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_RASTER,
                                      reinterpret_cast<const uint8_t*>(&list), sizeof(list)) != MICROPIXEL_STATUS_OK) {
            return 76;
        }
        list.header.target_buffer = 2U;
        if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_RASTER,
                                      reinterpret_cast<const uint8_t*>(&list),
                                      sizeof(list)) != MICROPIXEL_STATUS_NOT_FOUND) {
            return 77;
        }
    }

    // Destroy returns the in-flight buffer and retires the handle.
    micropixel_handle_request_t destroy{static_cast<uint16_t>(sizeof(destroy)), 0U, created.surface};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY, &destroy, sizeof(destroy), nullptr, 0U) !=
        MICROPIXEL_STATUS_OK) {
        return 69;
    }
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present), nullptr, 0U) !=
            MICROPIXEL_STATUS_NOT_FOUND ||
        Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY, &destroy, sizeof(destroy), nullptr, 0U) !=
            MICROPIXEL_STATUS_NOT_FOUND) {
        return 70;
    }

    // DESTROY itself counts as the return of the in-flight buffer: no
    // SURFACE_RELEASED for the retired handle may arrive afterwards.
    micropixel_event_t event{};
    for (uint32_t attempts = 0U; attempts < 8U; ++attempts) {
        const int32_t status = micropixel_event_wait(&event, sizeof(event), 0U);
        if (status == MICROPIXEL_STATUS_TIMEOUT) {
            break;
        }
        if (status != MICROPIXEL_STATUS_OK) {
            return 71;
        }
        if (event.service_id == MICROPIXEL_SERVICE_GRAPHICS &&
            event.event_id == MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED) {
            return 72;
        }
    }
    return 0;
}
