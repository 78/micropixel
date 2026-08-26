#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_DEVICE_CATALOG_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_DEVICE_CATALOG_HPP

#include <cstdint>

#include "device/devices.hpp"

namespace micropixel::platform::metalio_claw4 {

class DeviceCatalog final : public device::DeviceCatalogBackend {
   public:
    void Initialize(bool acceleration_available, bool magnetic_field_available);

    [[nodiscard]] uint32_t Generation() const override { return generation_; }
    [[nodiscard]] uint32_t Count() const override { return count_; }
    [[nodiscard]] int32_t GetByIndex(uint32_t index, micropixel_device_info_t& info_out) const override;
    [[nodiscard]] int32_t GetById(micropixel_device_id_t device, micropixel_device_info_t& info_out) const override;

   private:
    uint32_t count_{};
    uint32_t generation_{1U};
    bool acceleration_available_{};
    bool magnetic_field_available_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
