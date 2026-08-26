#ifndef MICROPIXEL_TEST_STUB_RUNTIME_SERVICES_TIMER_SERVICE_HPP
#define MICROPIXEL_TEST_STUB_RUNTIME_SERVICES_TIMER_SERVICE_HPP

#include <chrono>
#include <cstdint>

namespace micropixel::runtime {

class TimerService final {
   public:
    [[nodiscard]] uint64_t Now() const {
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
    [[nodiscard]] uint64_t FromGlobalTime(uint64_t timestamp_us) const { return timestamp_us; }

   private:
    std::chrono::steady_clock::time_point origin_{std::chrono::steady_clock::now()};
};

}  // namespace micropixel::runtime

#endif
