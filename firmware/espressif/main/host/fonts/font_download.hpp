// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace micropixel::host::fonts {
// One Host-owned request. Only the QUIC task writes response fields while
// pending. Release/acquire completion transfers the byte buffer to the Host.
struct FontDownload final {
    enum class State : uint8_t {
        kIdle,
        kRequested,
        kResolving,
        kResolved,
        kDownloadRequested,
        kDownloading,
        kReady,
        kFailed
    };
    std::atomic<State> state{State::kIdle};
    std::atomic<bool> cancelled{};
    std::atomic<uint8_t> progress{};
    std::array<char, 32U> locale{};
    std::array<char, 65U> app_id{};
    std::array<char, 32U> version{};
    std::array<uint8_t, 32U> sha256{};
    std::array<char, 160U> path{};
    uint8_t* bytes{};
    size_t size{};
};
}  // namespace micropixel::host::fonts
