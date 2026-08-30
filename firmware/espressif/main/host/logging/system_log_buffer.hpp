#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "abi/micropixel_abi.h"
#include "host/controller/control_types.hpp"
#include "runtime/guest_log_sink.hpp"

struct cJSON;

namespace micropixel::firmware::logging {

enum class LogSource : uint8_t {
    kHost,
    kApp,
};

enum class LogSourceFilter : uint8_t {
    kAll,
    kHost,
    kApp,
};

[[nodiscard]] const char* LogSourceText(LogSource source);
[[nodiscard]] bool ParseLogSourceFilter(std::string_view text, LogSourceFilter& filter_out);

struct LogRecordView final {
    LogSource source{LogSource::kHost};
    uint32_t level{MICROPIXEL_LOG_INFO};
    std::string_view app_id{};
    std::string_view message{};
};

using LogSubscriber = void (*)(void* context, const LogRecordView& record);

// Boot-lifetime, bounded log bus shared by Host logging, Guest logging and all
// control transports. Large storage lives in PSRAM; subscribers are fixed and
// callbacks run synchronously without holding the ring mutex.
class SystemLogBuffer final : public runtime::GuestLogSink {
   public:
    static constexpr size_t kMessageCapacity = MICROPIXEL_ABI_MAX_LOG_BYTES + 1U;
    static constexpr size_t kSubscriberCapacity = 4U;

    struct EntrySnapshot final {
        uint64_t sequence{};
        uint64_t timestamp_us{};
        uint32_t level{};
        LogSource source{LogSource::kHost};
        std::array<char, control::kAppIdCapacity> app_id{};
        std::array<char, kMessageCapacity> message{};
    };

    struct Page final {
        EntrySnapshot entry{};
        std::array<char, control::kCommandIdCapacity> session_id{};
        uint64_t next_sequence{};
        bool truncated{};
        bool has_more{};
        bool has_entry{};
    };

    SystemLogBuffer();
    ~SystemLogBuffer();
    SystemLogBuffer(const SystemLogBuffer&) = delete;
    SystemLogBuffer& operator=(const SystemLogBuffer&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool Subscribe(LogSubscriber subscriber, void* context);
    void UpdateAppLifecycle(const char* app_id);
    void WriteHostLog(uint32_t level, const char* bytes, size_t length, uint64_t timestamp_us);
    void WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                       uint64_t timestamp_us) override;
    [[nodiscard]] uint64_t NormalizeCursor(const char* session_id, uint64_t after_sequence) const;
    [[nodiscard]] Page ReadOne(const char* session_id, uint64_t after_sequence, LogSourceFilter filter) const;
    [[nodiscard]] cJSON* CreatePayload(uint64_t after_sequence, size_t maximum_entries, LogSourceFilter filter,
                                       uint64_t& next_cursor_out, bool& has_entries_out) const;

   private:
    struct Buffer;
    struct Subscriber final {
        LogSubscriber callback{};
        void* context{};
    };

    void Write(LogSource source, const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
               uint64_t timestamp_us);
    void Publish(const LogRecordView& record);

    mutable std::mutex mutex_;
    mutable std::mutex subscriber_mutex_;
    std::array<Subscriber, kSubscriberCapacity> subscribers_{};
    Buffer* buffer_{};
};

// Installs the sole system-level ESP_LOG vprintf interception. The previous
// writer remains the physical console sink while records are also published to
// SystemLogs(). Safe to call more than once.
[[nodiscard]] bool StartSystemLogCapture();
[[nodiscard]] SystemLogBuffer& SystemLogs();

}  // namespace micropixel::firmware::logging
