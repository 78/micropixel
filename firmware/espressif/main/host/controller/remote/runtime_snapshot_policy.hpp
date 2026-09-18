// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_RUNTIME_SNAPSHOT_POLICY_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_RUNTIME_SNAPSHOT_POLICY_HPP

#include <cstdint>

namespace micropixel::firmware::remote_control {

// Owned by the remote task. Connection setup and explicit reads bypass the
// idle refresh interval; lifecycle changes remain immediately observable.
class RuntimeSnapshotPolicy final {
   public:
    static constexpr int64_t kRefreshIntervalUs = 5LL * 60 * 1000000;

    [[nodiscard]] bool ShouldPublish(int64_t now_us, uint64_t generation, bool force = false) const {
        return force || !published_ || generation != published_generation_ ||
               now_us - last_published_us_ >= kRefreshIntervalUs;
    }

    // Record only snapshots accepted by the transport or its bounded outbox.
    void RecordPublished(int64_t now_us, uint64_t generation) {
        published_ = true;
        published_generation_ = generation;
        last_published_us_ = now_us;
    }

   private:
    bool published_{};
    uint64_t published_generation_{};
    int64_t last_published_us_{};
};

}  // namespace micropixel::firmware::remote_control

#endif
