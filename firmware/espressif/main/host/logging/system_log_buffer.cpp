#include "host/logging/system_log_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_private/log_lock.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace micropixel::firmware::logging {
namespace {

constexpr size_t kCapacity = 1024U;
constexpr size_t kResponseJsonBudget = 60U * 1024U;
constexpr size_t kStackFormatCapacity = 512U;

struct Entry final {
    uint64_t sequence{};
    uint64_t timestamp_us{};
    uint32_t level{};
    LogSource source{LogSource::kHost};
    std::array<char, control::kAppIdCapacity> app_id{};
    std::array<char, SystemLogBuffer::kMessageCapacity> message{};
};

struct HeapCapsDeleter final {
    void operator()(char* storage) const { heap_caps_free(storage); }
};

using FormatStorage = std::unique_ptr<char[], HeapCapsDeleter>;

vprintf_like_t gConsoleWriter = nullptr;
bool gCaptureStarted = false;
std::mutex gCaptureMutex;

bool Matches(LogSource source, LogSourceFilter filter) {
    return filter == LogSourceFilter::kAll || (filter == LogSourceFilter::kHost && source == LogSource::kHost) ||
           (filter == LogSourceFilter::kApp && source == LogSource::kApp);
}

const char* LevelText(uint32_t level) {
    switch (level) {
        case MICROPIXEL_LOG_DEBUG:
            return "debug";
        case MICROPIXEL_LOG_INFO:
            return "info";
        case MICROPIXEL_LOG_WARNING:
            return "warning";
        case MICROPIXEL_LOG_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

uint32_t HostLevel(std::string_view message) {
    const size_t plain = message.starts_with("\033[") ? message.find('m') + 1U : 0U;
    if (plain < message.size()) {
        switch (message[plain]) {
            case 'D':
                return MICROPIXEL_LOG_DEBUG;
            case 'W':
                return MICROPIXEL_LOG_WARNING;
            case 'E':
                return MICROPIXEL_LOG_ERROR;
            default:
                break;
        }
    }
    return MICROPIXEL_LOG_INFO;
}

void WriteConsole(const char* format, ...) {
    if (gConsoleWriter == nullptr) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)gConsoleWriter(format, arguments);
    va_end(arguments);
}

int CaptureHostLog(const char* format, va_list arguments) {
    va_list console_arguments;
    va_list capture_arguments;
    va_list expanded_arguments;
    va_copy(console_arguments, arguments);
    va_copy(capture_arguments, arguments);
    va_copy(expanded_arguments, arguments);
    const int result = gConsoleWriter != nullptr ? gConsoleWriter(format, console_arguments) : 0;
    va_end(console_arguments);

    std::array<char, kStackFormatCapacity> formatted{};
    const int formatted_size = std::vsnprintf(formatted.data(), formatted.size(), format, capture_arguments);
    va_end(capture_arguments);
    if (formatted_size <= 0) {
        va_end(expanded_arguments);
        return result;
    }

    const size_t required_size = static_cast<size_t>(formatted_size);
    std::span<const char> captured(formatted.data(), std::min(required_size, formatted.size() - 1U));
    FormatStorage expanded;
    if (required_size >= formatted.size()) {
        expanded.reset(static_cast<char*>(heap_caps_malloc(required_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (expanded != nullptr) {
            const int expanded_size = std::vsnprintf(expanded.get(), required_size + 1U, format, expanded_arguments);
            if (expanded_size > 0) {
                captured =
                    std::span<const char>(expanded.get(), std::min(static_cast<size_t>(expanded_size), required_size));
            }
        }
    }
    va_end(expanded_arguments);

    SystemLogs().WriteHostLog(HostLevel(std::string_view(captured.data(), captured.size())), captured.data(),
                              captured.size(), static_cast<uint64_t>(esp_timer_get_time()));
    return result;
}

}  // namespace

struct SystemLogBuffer::Buffer final {
    std::array<Entry, kCapacity> entries{};
    std::array<char, control::kCommandIdCapacity> session_id{};
    std::array<char, control::kAppIdCapacity> active_app_id{};
    size_t start{};
    size_t count{};
    uint64_t next_sequence{1U};
};

const char* LogSourceText(LogSource source) { return source == LogSource::kApp ? "app" : "host"; }

bool ParseLogSourceFilter(std::string_view text, LogSourceFilter& filter_out) {
    if (text.empty() || text == "app") {
        filter_out = LogSourceFilter::kApp;
        return true;
    }
    if (text == "all") {
        filter_out = LogSourceFilter::kAll;
        return true;
    }
    if (text == "host") {
        filter_out = LogSourceFilter::kHost;
        return true;
    }
    return false;
}

SystemLogBuffer::SystemLogBuffer() {
    static_assert(sizeof(Entry) * kCapacity < 2U * 1024U * 1024U,
                  "System log ring must remain a bounded PSRAM allocation");
    void* storage = heap_caps_calloc(1U, sizeof(Buffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage != nullptr) {
        buffer_ = std::construct_at(static_cast<Buffer*>(storage));
        const uint64_t boot_id = (static_cast<uint64_t>(esp_random()) << 32U) | esp_random();
        std::snprintf(buffer_->session_id.data(), buffer_->session_id.size(), "%016llx",
                      static_cast<unsigned long long>(boot_id));
    }
}

SystemLogBuffer::~SystemLogBuffer() {
    if (buffer_ != nullptr) {
        std::destroy_at(buffer_);
        heap_caps_free(buffer_);
    }
}

bool SystemLogBuffer::valid() const { return buffer_ != nullptr; }

bool SystemLogBuffer::Subscribe(LogSubscriber subscriber, void* context) {
    if (subscriber == nullptr) {
        return false;
    }
    std::lock_guard lock(subscriber_mutex_);
    for (Subscriber& slot : subscribers_) {
        if (slot.callback == subscriber && slot.context == context) {
            return true;
        }
        if (slot.callback == nullptr) {
            slot = {.callback = subscriber, .context = context};
            return true;
        }
    }
    return false;
}

void SystemLogBuffer::UpdateAppLifecycle(const char* app_id) {
    if (buffer_ == nullptr) {
        return;
    }
    std::lock_guard lock(mutex_);
    std::snprintf(buffer_->active_app_id.data(), buffer_->active_app_id.size(), "%s", app_id != nullptr ? app_id : "");
}

void SystemLogBuffer::WriteHostLog(uint32_t level, const char* bytes, size_t length, uint64_t timestamp_us) {
    Write(LogSource::kHost, nullptr, level, reinterpret_cast<const uint8_t*>(bytes), length, timestamp_us);
}

void SystemLogBuffer::WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                                    uint64_t timestamp_us) {
    Write(LogSource::kApp, app_id, level, bytes, length, timestamp_us);
    esp_log_impl_lock();
    WriteConsole("[APP %s] %.*s\n", app_id != nullptr ? app_id : "unknown", static_cast<int>(length),
                 bytes != nullptr ? reinterpret_cast<const char*>(bytes) : "");
    esp_log_impl_unlock();
}

void SystemLogBuffer::Write(LogSource source, const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                            uint64_t timestamp_us) {
    if (buffer_ == nullptr || (bytes == nullptr && length != 0U)) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        const size_t index = buffer_->count < buffer_->entries.size()
                                 ? (buffer_->start + buffer_->count) % buffer_->entries.size()
                                 : buffer_->start;
        if (buffer_->count < buffer_->entries.size()) {
            ++buffer_->count;
        } else {
            buffer_->start = (buffer_->start + 1U) % buffer_->entries.size();
        }
        Entry& entry = buffer_->entries[index];
        entry = {};
        entry.sequence = buffer_->next_sequence++;
        entry.timestamp_us = timestamp_us;
        entry.level = level;
        entry.source = source;
        std::snprintf(entry.app_id.data(), entry.app_id.size(), "%s", app_id != nullptr ? app_id : "");
        const size_t copied = std::min(length, entry.message.size() - 1U);
        if (copied != 0U) {
            std::memcpy(entry.message.data(), bytes, copied);
        }
        entry.message[copied] = '\0';
    }
    Publish({.source = source,
             .level = level,
             .app_id = app_id != nullptr ? std::string_view(app_id) : std::string_view{},
             .message = bytes != nullptr ? std::string_view(reinterpret_cast<const char*>(bytes), length)
                                         : std::string_view{}});
}

void SystemLogBuffer::Publish(const LogRecordView& record) {
    std::array<Subscriber, kSubscriberCapacity> subscribers{};
    {
        std::lock_guard lock(subscriber_mutex_);
        subscribers = subscribers_;
    }
    for (const Subscriber& subscriber : subscribers) {
        if (subscriber.callback != nullptr) {
            subscriber.callback(subscriber.context, record);
        }
    }
}

uint64_t SystemLogBuffer::NormalizeCursor(const char* session_id, uint64_t after_sequence) const {
    std::lock_guard lock(mutex_);
    return buffer_ != nullptr && session_id != nullptr && std::strcmp(session_id, buffer_->session_id.data()) == 0
               ? after_sequence
               : 0U;
}

SystemLogBuffer::Page SystemLogBuffer::ReadOne(const char* session_id, uint64_t after_sequence,
                                               LogSourceFilter filter) const {
    Page page{};
    std::lock_guard lock(mutex_);
    if (buffer_ == nullptr) {
        return page;
    }
    page.session_id = buffer_->session_id;
    const bool same_session = session_id != nullptr && std::strcmp(session_id, buffer_->session_id.data()) == 0;
    const uint64_t normalized_after = same_session ? after_sequence : 0U;
    page.next_sequence = normalized_after;
    if (buffer_->count != 0U) {
        const uint64_t oldest_sequence = buffer_->entries[buffer_->start].sequence;
        page.truncated = oldest_sequence > normalized_after && oldest_sequence - normalized_after > 1U;
    }
    size_t matched_offset = buffer_->count;
    for (size_t offset = 0U; offset < buffer_->count; ++offset) {
        const Entry& source = buffer_->entries[(buffer_->start + offset) % buffer_->entries.size()];
        if (source.sequence <= normalized_after) {
            continue;
        }
        page.next_sequence = source.sequence;
        if (!Matches(source.source, filter)) {
            continue;
        }
        page.entry.sequence = source.sequence;
        page.entry.timestamp_us = source.timestamp_us;
        page.entry.level = source.level;
        page.entry.source = source.source;
        page.entry.app_id = source.app_id;
        page.entry.message = source.message;
        page.has_entry = true;
        matched_offset = offset;
        break;
    }
    if (page.has_entry) {
        for (size_t offset = matched_offset + 1U; offset < buffer_->count; ++offset) {
            const Entry& candidate = buffer_->entries[(buffer_->start + offset) % buffer_->entries.size()];
            if (Matches(candidate.source, filter)) {
                page.has_more = true;
                break;
            }
        }
    }
    return page;
}

cJSON* SystemLogBuffer::CreatePayload(uint64_t after_sequence, size_t maximum_entries, LogSourceFilter filter,
                                      uint64_t& next_cursor_out, bool& has_entries_out) const {
    cJSON* result = cJSON_CreateObject();
    cJSON* entries = result != nullptr ? cJSON_AddArrayToObject(result, "entries") : nullptr;
    if (result == nullptr || entries == nullptr) {
        cJSON_Delete(result);
        return nullptr;
    }
    uint64_t next_cursor = after_sequence;
    bool truncated = false;
    bool has_more = false;
    size_t appended = 0U;
    size_t estimated_json_bytes = 256U;
    {
        std::lock_guard lock(mutex_);
        if (buffer_ != nullptr) {
            (void)cJSON_AddStringToObject(result, "appSessionId", buffer_->session_id.data());
            if (buffer_->count != 0U) {
                const uint64_t oldest_sequence = buffer_->entries[buffer_->start].sequence;
                truncated = oldest_sequence > after_sequence && oldest_sequence - after_sequence > 1U;
            }
            for (size_t offset = 0U; offset < buffer_->count; ++offset) {
                const Entry& entry = buffer_->entries[(buffer_->start + offset) % buffer_->entries.size()];
                if (entry.sequence <= after_sequence) {
                    continue;
                }
                if (!Matches(entry.source, filter)) {
                    next_cursor = entry.sequence;
                    continue;
                }
                if (appended >= maximum_entries) {
                    has_more = true;
                    break;
                }
                const size_t entry_json_upper_bound =
                    (std::strlen(entry.app_id.data()) + std::strlen(entry.message.data())) * 6U + 288U;
                if (appended != 0U && estimated_json_bytes + entry_json_upper_bound > kResponseJsonBudget) {
                    has_more = true;
                    break;
                }
                cJSON* item = cJSON_CreateObject();
                if (item == nullptr) {
                    continue;
                }
                (void)cJSON_AddNumberToObject(item, "sequence", static_cast<double>(entry.sequence));
                (void)cJSON_AddNumberToObject(item, "timestampUs", static_cast<double>(entry.timestamp_us));
                (void)cJSON_AddStringToObject(item, "level", LevelText(entry.level));
                (void)cJSON_AddStringToObject(item, "source", LogSourceText(entry.source));
                (void)cJSON_AddStringToObject(item, "appId", entry.app_id.data());
                (void)cJSON_AddStringToObject(item, "message", entry.message.data());
                cJSON_AddItemToArray(entries, item);
                next_cursor = entry.sequence;
                ++appended;
                estimated_json_bytes += entry_json_upper_bound;
            }
        }
    }
    (void)cJSON_AddNumberToObject(result, "nextSequence", static_cast<double>(next_cursor));
    (void)cJSON_AddBoolToObject(result, "truncated", truncated);
    (void)cJSON_AddBoolToObject(result, "hasMore", has_more);
    next_cursor_out = next_cursor;
    has_entries_out = appended != 0U;
    return result;
}

SystemLogBuffer& SystemLogs() {
    static SystemLogBuffer logs;
    return logs;
}

bool StartSystemLogCapture() {
    std::lock_guard lock(gCaptureMutex);
    if (gCaptureStarted) {
        return true;
    }
    if (!SystemLogs().valid()) {
        return false;
    }
    gConsoleWriter = &vprintf;
    gConsoleWriter = esp_log_set_vprintf(CaptureHostLog);
    gCaptureStarted = true;
    return true;
}

}  // namespace micropixel::firmware::logging
