#include "abi/micropixel_abi.h"
#include "device/random.hpp"
#include "esp_random.h"
#include "platform/configured_backends.hpp"

namespace micropixel::platform {
namespace {

class EspRandomBackend final : public device::RandomBackend {
   public:
    [[nodiscard]] int32_t GetU32(uint32_t& value_out) override {
        value_out = esp_random();
        return MICROPIXEL_STATUS_OK;
    }
};

}  // namespace

device::RandomBackend& ConfiguredRandomBackend() {
    static EspRandomBackend backend;
    return backend;
}

}  // namespace micropixel::platform
