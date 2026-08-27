#include <cstdlib>
#include <cstring>

#include "platform/boards/metalio-claw4/device_catalog.hpp"
#include "platform/boards/metalio-claw4/peripheral_ids.hpp"

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

}  // namespace

int main() {
    using micropixel::platform::metalio_claw4::DeviceCatalog;
    namespace ids = micropixel::platform::metalio_claw4::peripheral_ids;

    DeviceCatalog catalog;
    catalog.Initialize(true, true);
    Require(catalog.Count() == 21U);
    micropixel_device_info_t info{};
    Require(catalog.GetByIndex(3U, info) == MICROPIXEL_STATUS_OK && info.device == ids::kAcceleration);
    Require(catalog.GetByIndex(4U, info) == MICROPIXEL_STATUS_OK && info.device == ids::kMagneticField);
    Require(catalog.GetByIndex(5U, info) == MICROPIXEL_STATUS_OK && info.device == ids::Gpio(5U));
    Require(catalog.GetByIndex(18U, info) == MICROPIXEL_STATUS_OK && info.device == ids::Gpio(48U));
    Require(catalog.GetByIndex(19U, info) == MICROPIXEL_STATUS_OK && info.device == ids::kHaptics);
    Require(catalog.GetByIndex(20U, info) == MICROPIXEL_STATUS_OK && info.device == ids::kPower);
    Require(catalog.GetById(ids::Gpio(35U), info) == MICROPIXEL_STATUS_OK && std::strcmp(info.name, "GPIO 35") == 0);
    Require(catalog.GetByIndex(21U, info) == MICROPIXEL_STATUS_NOT_FOUND);

    catalog.Initialize(false, false);
    Require(catalog.Count() == 19U);
    Require(catalog.GetByIndex(3U, info) == MICROPIXEL_STATUS_OK && info.device == ids::Gpio(5U));
    Require(catalog.GetById(ids::kAcceleration, info) == MICROPIXEL_STATUS_NOT_FOUND);
    return 0;
}
