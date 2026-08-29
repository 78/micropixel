#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "abi/micropixel_abi.h"
#include "host/controller/control_types.hpp"
#include "runtime/guest_log_sink.hpp"

struct cJSON;

namespace micropixel::firmware::control {

class GuestLogBuffer final : public runtime::GuestLogSink {
   public:
    static constexpr size_t kMessageCapacity = MICROPIXEL_ABI_MAX_LOG_BYTES + 1U;

    struct EntrySnapshot final {
        uint64_t sequence{};
        uint64_t timestamp_us{};
        uint32_t level{};
        std::array<char, kAppIdCapacity> app_id{};
        std::array<char, kMessageCapacity> message{};
    };

    struct Page final {
        EntrySnapshot entry{};
        std::array<char, kCommandIdCapacity> session_id{};
        uint64_t next_sequence{};
        bool truncated{};
        bool has_more{};
        bool has_entry{};
    };

    GuestLogBuffer();
    ~GuestLogBuffer();
    GuestLogBuffer(const GuestLogBuffer&) = delete;
    GuestLogBuffer& operator=(const GuestLogBuffer&) = delete;

    [[nodiscard]] bool valid() const;
    void ResetSession(const char* session_id);
    void UpdateAppLifecycle(const char* app_id);
    void Write(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length, uint64_t timestamp_us);
    void WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                       uint64_t timestamp_us) override {
        Write(app_id, level, bytes, length, timestamp_us);
    }
    [[nodiscard]] uint64_t NormalizeCursor(const char* session_id, uint64_t after_sequence) const;
    [[nodiscard]] Page ReadOne(const char* session_id, uint64_t after_sequence) const;
    [[nodiscard]] cJSON* CreatePayload(uint64_t after_sequence, size_t maximum_entries, uint64_t& next_cursor_out,
                                       bool& has_entries_out) const;

   private:
    struct Buffer;
    mutable std::mutex mutex_;
    Buffer* buffer_{};
};

}  // namespace micropixel::firmware::control
