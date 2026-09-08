#include "runtime/service_binding.hpp"

namespace micropixel::runtime {

void RequireOk(int32_t status, const char* operation) {
    if (status != MICROPIXEL_STATUS_OK) {
        micropixel::runtime::Panic(operation, status);
    }
}
int32_t OpenService(ServiceCache& cache, uint32_t service_id, uint16_t interface_major, uint16_t minimum_minor) {
    if (!cache.attempted) {
        cache.attempted = true;
        cache.status = micropixel_service_open(service_id, MICROPIXEL_INTERFACE_VERSION(interface_major, minimum_minor),
                                               &cache.info, sizeof(cache.info));
        if (cache.status == MICROPIXEL_STATUS_OK &&
            (cache.info.size < sizeof(cache.info) || cache.info.service_id != service_id ||
             cache.info.service_handle == 0U || cache.info.interface_major != interface_major ||
             cache.info.interface_minor < minimum_minor)) {
            cache.status = MICROPIXEL_STATUS_VERSION_MISMATCH;
        }
    }
    return cache.status;
}

int32_t CallService(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size, void* response,
                    uint32_t response_capacity, uint32_t& response_size_out) {
    if (cache.status != MICROPIXEL_STATUS_OK || !cache.attempted) {
        return cache.attempted ? cache.status : MICROPIXEL_STATUS_INTERNAL;
    }
    return micropixel_service_call(cache.info.service_handle, method_id, static_cast<const uint8_t*>(request),
                                   request_size, static_cast<uint8_t*>(response), response_capacity,
                                   &response_size_out);
}

int32_t CallVoid(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size) {
    uint32_t response_size = 0U;
    int32_t status = CallService(cache, method_id, request, request_size, nullptr, 0U, response_size);
    return status == MICROPIXEL_STATUS_OK && response_size != 0U ? MICROPIXEL_STATUS_INTERNAL : status;
}

}  // namespace micropixel::runtime
