#ifndef MICROPIXEL_RUNTIME_ABI_SERVICE_REGISTRY_HPP
#define MICROPIXEL_RUNTIME_ABI_SERVICE_REGISTRY_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::runtime {

struct ServiceDescriptor final {
    uint32_t service_id{};
    uint16_t interface_major{};
    uint16_t interface_minor{};
    uint32_t flags{};
    uint64_t capabilities{};
    uint32_t max_request_bytes{};
    uint32_t max_response_bytes{};
    uint32_t max_submit_bytes{};
};

class ServiceHandler {
   public:
    ServiceHandler() = default;
    ServiceHandler(const ServiceHandler&) = delete;
    ServiceHandler& operator=(const ServiceHandler&) = delete;
    virtual ~ServiceHandler() = default;

    [[nodiscard]] virtual ServiceDescriptor Describe() const = 0;
    [[nodiscard]] virtual int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                       uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) = 0;
    [[nodiscard]] virtual int32_t Submit(uint32_t channel_id, const uint8_t* bytes, uint32_t length) {
        (void)channel_id;
        (void)bytes;
        (void)length;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
};

// Fixed-capacity registry for one Guest. service_open resolves a stable slot
// once; call and submit use that handle without service-name lookup or heap use.
class ServiceRegistry final {
   public:
    template <typename... Endpoints>
    explicit ServiceRegistry(Endpoints&... endpoints) : handlers_{&endpoints...}, service_count_(sizeof...(endpoints)) {
        static_assert(sizeof...(endpoints) > 0U, "ServiceRegistry requires at least one endpoint");
        static_assert(sizeof...(endpoints) <= kMaxServiceCount, "ServiceRegistry endpoint capacity exceeded");
        // Descriptors are immutable for one Guest lifetime. Cache them once so
        // call/submit paths remain a bounds check, array lookup and virtual call.
        for (uint32_t index = 0U; index < service_count_; ++index) {
            descriptors_[index] = handlers_[index]->Describe();
        }
    }

    [[nodiscard]] int32_t Open(uint32_t service_id, uint32_t required_interface_version,
                               micropixel_service_info_t& info_out, uint32_t info_capacity) const;
    [[nodiscard]] int32_t Call(micropixel_service_handle_t service, uint32_t method_id, const uint8_t* request,
                               uint32_t request_size, uint8_t* response, uint32_t response_capacity,
                               uint32_t& response_size_out) const;
    [[nodiscard]] int32_t Submit(micropixel_service_handle_t service, uint32_t channel_id, const uint8_t* bytes,
                                 uint32_t length) const;

   private:
    static constexpr uint32_t kMaxServiceCount = 16U;

    [[nodiscard]] bool Resolve(micropixel_service_handle_t service, ServiceHandler*& handler_out,
                               const ServiceDescriptor*& descriptor_out) const;

    ServiceHandler* handlers_[kMaxServiceCount]{};
    ServiceDescriptor descriptors_[kMaxServiceCount]{};
    uint32_t service_count_{};
};

}  // namespace micropixel::runtime

#endif
