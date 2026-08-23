#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {

bool ServiceRegistry::Resolve(micropixel_service_handle_t service, ServiceHandler*& handler_out,
                              const ServiceDescriptor*& descriptor_out) const {
    if (service == 0U || service > service_count_) {
        return false;
    }
    const uint32_t index = service - 1U;
    handler_out = handlers_[index];
    descriptor_out = &descriptors_[index];
    return true;
}

int32_t ServiceRegistry::Open(uint32_t service_id, uint32_t required_interface_version,
                              micropixel_service_info_t& info_out, uint32_t info_capacity) const {
    if (info_capacity < sizeof(micropixel_service_info_t)) {
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }

    const uint16_t required_major = static_cast<uint16_t>(required_interface_version >> 16U);
    const uint16_t required_minor = static_cast<uint16_t>(required_interface_version);
    for (uint32_t index = 0U; index < service_count_; ++index) {
        const ServiceDescriptor& descriptor = descriptors_[index];
        if (descriptor.service_id != service_id) {
            continue;
        }
        if (descriptor.interface_major != required_major || descriptor.interface_minor < required_minor) {
            return MICROPIXEL_STATUS_VERSION_MISMATCH;
        }

        info_out = {};
        info_out.size = sizeof(info_out);
        info_out.service_id = descriptor.service_id;
        info_out.handle = index + 1U;
        info_out.interface_major = descriptor.interface_major;
        info_out.interface_minor = descriptor.interface_minor;
        info_out.flags = descriptor.flags;
        info_out.capabilities = descriptor.capabilities;
        info_out.max_request_bytes = descriptor.max_request_bytes;
        info_out.max_response_bytes = descriptor.max_response_bytes;
        info_out.max_submit_bytes = descriptor.max_submit_bytes;
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
}

int32_t ServiceRegistry::Call(micropixel_service_handle_t service, uint32_t method_id, const uint8_t* request,
                              uint32_t request_size, uint8_t* response, uint32_t response_capacity,
                              uint32_t& response_size_out) const {
    response_size_out = 0U;
    ServiceHandler* handler = nullptr;
    const ServiceDescriptor* descriptor = nullptr;
    if (!Resolve(service, handler, descriptor)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if ((descriptor->flags & MICROPIXEL_SERVICE_FLAG_CALL) == 0U) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if ((request == nullptr && request_size != 0U) || (response == nullptr && response_capacity != 0U) ||
        request_size > descriptor->max_request_bytes) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return handler->Call(method_id, request, request_size, response, response_capacity, response_size_out);
}

int32_t ServiceRegistry::Submit(micropixel_service_handle_t service, uint32_t channel_id, const uint8_t* bytes,
                                uint32_t length) const {
    ServiceHandler* handler = nullptr;
    const ServiceDescriptor* descriptor = nullptr;
    if (!Resolve(service, handler, descriptor)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if ((descriptor->flags & MICROPIXEL_SERVICE_FLAG_SUBMIT) == 0U) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (bytes == nullptr || length == 0U || length > descriptor->max_submit_bytes) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return handler->Submit(channel_id, bytes, length);
}

}  // namespace micropixel::runtime
