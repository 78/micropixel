#ifndef MICROPIXEL_DEVICE_RANDOM_HPP
#define MICROPIXEL_DEVICE_RANDOM_HPP

#include <cstdint>

namespace micropixel::device {

class Random {
   public:
    virtual ~Random() = default;

    [[nodiscard]] virtual int32_t GetU32(uint32_t& value_out) = 0;
};

}  // namespace micropixel::device

#endif
