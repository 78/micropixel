#ifndef MICROPIXEL_DEVICE_RANDOM_HPP
#define MICROPIXEL_DEVICE_RANDOM_HPP

#include <cstdint>

namespace micropixel::device {

class RandomBackend {
   public:
    virtual ~RandomBackend() = default;

    [[nodiscard]] virtual int32_t GetU32(uint32_t& value_out) = 0;
};

}  // namespace micropixel::device

#endif
