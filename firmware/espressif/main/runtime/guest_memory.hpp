#ifndef MICROPIXEL_RUNTIME_GUEST_MEMORY_HPP
#define MICROPIXEL_RUNTIME_GUEST_MEMORY_HPP

#include <cstdint>

namespace micropixel::runtime {
// Resolves a Guest linear-memory range to a Host pointer. Bound by the session
// once the WAMR instance exists; the Host never trusts a Guest offset without it.
struct GuestMemoryAccess final {
    void* context{};
    bool (*resolve)(void* context, uint32_t offset, uint32_t length, uint8_t** host_out){};
    // The linear-memory base cannot move for the instance lifetime (the
    // Bundle declared PINNED_MEMORY). Without it a resolved pointer is only
    // good until the Guest's next memory.grow, so no GUEST_BUFFERS Direct
    // Surface may be created: its buffers stay in flight across calls.
    bool stable_base{};
};

}  // namespace micropixel::runtime
#endif
