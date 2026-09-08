#ifndef MICROPIXEL_GUEST_RUNTIME_SERVICE_BINDING_HPP
#define MICROPIXEL_GUEST_RUNTIME_SERVICE_BINDING_HPP

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"
#include "sdk/error.hpp"

namespace micropixel::runtime {

struct ServiceCache final {
    micropixel_service_info_t info{};
    int32_t status{MICROPIXEL_STATUS_INTERNAL};
    bool attempted{};
};

// Builtins lower to memory.copy / memory.fill (Guests always build with
// -mbulk-memory); -ffreestanding would otherwise keep the byte loops.
inline void CopyBytes(void* destination, const void* source, uint32_t length) {
    __builtin_memcpy(destination, source, length);
}

inline void ZeroBytes(void* destination, uint32_t length) { __builtin_memset(destination, 0, length); }

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

void RequireOk(int32_t status, const char* operation);
inline micropixel::Error ErrorFromStatus(int32_t status) {
    using micropixel::Error;
    using micropixel::ErrorCode;
    switch (status) {
        case MICROPIXEL_STATUS_INVALID_ARGUMENT:
        case MICROPIXEL_STATUS_INVALID_MEMORY:
            return Error{ErrorCode::kInvalidArgument};
        case MICROPIXEL_STATUS_CLOSED:
        case MICROPIXEL_STATUS_STALE_STATE:
            return Error{ErrorCode::kInvalidState};
        case MICROPIXEL_STATUS_UNSUPPORTED:
            return Error{ErrorCode::kUnsupported};
        case MICROPIXEL_STATUS_RESOURCE_EXHAUSTED:
            return Error{ErrorCode::kResourceExhausted};
        case MICROPIXEL_STATUS_NOT_FOUND:
            return Error{ErrorCode::kNotFound};
        case MICROPIXEL_STATUS_PERMISSION_DENIED:
            return Error{ErrorCode::kPermissionDenied};
        case MICROPIXEL_STATUS_BUFFER_TOO_SMALL:
            return Error{ErrorCode::kBufferTooSmall};
        case MICROPIXEL_STATUS_RATE_LIMITED:
            return Error{ErrorCode::kRateLimited};
        case MICROPIXEL_STATUS_WOULD_BLOCK:
            return Error{ErrorCode::kWouldBlock};
        case MICROPIXEL_STATUS_CANCELLED:
            return Error{ErrorCode::kCancelled};
        default:
            return Error{ErrorCode::kInternal};
    }
}

int32_t OpenService(ServiceCache& cache, uint32_t service_id, uint16_t interface_major, uint16_t minimum_minor);
int32_t CallService(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size, void* response,
                    uint32_t response_capacity, uint32_t& response_size_out);
int32_t CallVoid(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size);

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_SERVICE_BINDING_HPP
