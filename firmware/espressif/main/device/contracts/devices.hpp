#ifndef MICROPIXEL_DEVICE_DEVICES_HPP
#define MICROPIXEL_DEVICE_DEVICES_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

// Platform-owned, fixed-capacity catalog. Device IDs are opaque to Guests and
// remain stable while an entry is present. Index is enumeration state only.
class DeviceCatalog {
   public:
    virtual ~DeviceCatalog() = default;
    DeviceCatalog(const DeviceCatalog&) = delete;
    DeviceCatalog& operator=(const DeviceCatalog&) = delete;

    [[nodiscard]] virtual uint32_t Generation() const = 0;
    [[nodiscard]] virtual uint32_t Count() const = 0;
    [[nodiscard]] virtual int32_t GetByIndex(uint32_t index, micropixel_device_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t GetById(micropixel_device_id_t device, micropixel_device_info_t& info_out) const = 0;

   protected:
    DeviceCatalog() = default;
};

}  // namespace micropixel::device

#endif
