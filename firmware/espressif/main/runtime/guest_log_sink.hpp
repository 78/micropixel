#ifndef MICROPIXEL_RUNTIME_GUEST_LOG_SINK_HPP
#define MICROPIXEL_RUNTIME_GUEST_LOG_SINK_HPP

#include <cstddef>
#include <cstdint>

namespace micropixel::runtime {

// Optional Host-owned destination for Guest log records. Runtime only knows
// this contract; concrete storage and transport remain outside the Runtime.
// Implementations must copy the supplied bytes before returning.
class GuestLogSink {
   public:
    GuestLogSink() = default;
    GuestLogSink(const GuestLogSink&) = delete;
    GuestLogSink& operator=(const GuestLogSink&) = delete;
    virtual ~GuestLogSink() = default;

    virtual void WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                               uint64_t timestamp_us) = 0;
};

}  // namespace micropixel::runtime

#endif
