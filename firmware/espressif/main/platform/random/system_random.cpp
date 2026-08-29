#include "platform/random/system_random.hpp"

#include "abi/micropixel_abi.h"
#include "device/contracts/random.hpp"
#include "esp_random.h"

namespace micropixel::platform::random {
namespace {

class EspRandom final : public device::Random {
   public:
    [[nodiscard]] int32_t GetU32(uint32_t& value_out) override {
        value_out = esp_random();
        return MICROPIXEL_STATUS_OK;
    }
};

}  // namespace

device::Random& SystemRandom() {
    static EspRandom random;
    return random;
}

}  // namespace micropixel::platform::random
