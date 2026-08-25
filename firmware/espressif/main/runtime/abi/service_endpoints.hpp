#ifndef MICROPIXEL_RUNTIME_ABI_SERVICE_ENDPOINTS_HPP
#define MICROPIXEL_RUNTIME_ABI_SERVICE_ENDPOINTS_HPP

#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {

class GuestContext;

class SystemServiceEndpoint final : public ServiceHandler {
   public:
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;
};

class TimerServiceEndpoint final : public ServiceHandler {
   public:
    explicit TimerServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class StorageServiceEndpoint final : public ServiceHandler {
   public:
    explicit StorageServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class ResourceServiceEndpoint final : public ServiceHandler {
   public:
    explicit ResourceServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class RandomServiceEndpoint final : public ServiceHandler {
   public:
    explicit RandomServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class GraphicsServiceEndpoint final : public ServiceHandler {
   public:
    explicit GraphicsServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;
    [[nodiscard]] int32_t Submit(uint32_t channel_id, const uint8_t* bytes, uint32_t length) override;

   private:
    GuestContext& context_;
};

class InputServiceEndpoint final : public ServiceHandler {
   public:
    explicit InputServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class AudioServiceEndpoint final : public ServiceHandler {
   public:
    explicit AudioServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

}  // namespace micropixel::runtime

#endif
