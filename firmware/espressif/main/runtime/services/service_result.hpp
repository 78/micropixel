#ifndef MICROPIXEL_RUNTIME_SERVICES_SERVICE_RESULT_HPP
#define MICROPIXEL_RUNTIME_SERVICES_SERVICE_RESULT_HPP

#include <cstdint>
#include <expected>

#include "abi/micropixel_abi.h"

namespace micropixel::runtime {

struct ServiceFailure final {
    int32_t status{MICROPIXEL_STATUS_INTERNAL};
    uint32_t detail{};
};

template <typename Value>
using ServiceResult = std::expected<Value, ServiceFailure>;

template <typename Value>
ServiceResult<Value> FailService(int32_t status, uint32_t detail = 0U) {
    return std::unexpected(ServiceFailure{status, detail});
}

inline ServiceResult<void> ServiceStatus(int32_t status) {
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(status);
    }
    return {};
}

}  // namespace micropixel::runtime

#endif
