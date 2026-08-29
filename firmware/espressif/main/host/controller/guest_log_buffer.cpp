#include "host/controller/guest_log_buffer.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "abi/micropixel_abi.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "host/controller/control_types.hpp"

namespace micropixel::firmware::control {
namespace {

constexpr size_t kCapacity = 1024U;
constexpr size_t kResponseJsonBudget = 60U * 1024U;

struct Entry final {
    uint64_t sequence{};
    uint64_t timestamp_us{};
    uint32_t level{};
    std::array<char, control::kAppIdCapacity> app_id{};
    std::array<char, GuestLogBuffer::kMessageCapacity> message{};
};

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

}  // namespace

struct GuestLogBuffer::Buffer final {
    std::array<Entry, kCapacity> entries{};
    std::array<char, control::kCommandIdCapacity> session_id{};
    std::array<char, control::kAppIdCapacity> active_app_id{};
    size_t start{};
    size_t count{};
    uint64_t next_sequence{1U};
    uint64_t session_generation{};
};

GuestLogBuffer::GuestLogBuffer() {
    static_assert(sizeof(Entry) * kCapacity < 2U * 1024U * 1024U,
                  "Guest log ring must remain a bounded PSRAM allocation");
    void* storage = heap_caps_calloc(1U, sizeof(Buffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage != nullptr) {
        buffer_ = std::construct_at(static_cast<Buffer*>(storage));
    }
}

GuestLogBuffer::~GuestLogBuffer() {
    if (buffer_ != nullptr) {
        std::destroy_at(buffer_);
        heap_caps_free(buffer_);
    }
}

bool GuestLogBuffer::valid() const { return buffer_ != nullptr; }

void GuestLogBuffer::ResetSession(const char* session_id) {
    if (buffer_ == nullptr || session_id == nullptr) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (std::strcmp(buffer_->session_id.data(), session_id) == 0) {
        return;
    }
    buffer_->entries = {};
    buffer_->start = 0U;
    buffer_->count = 0U;
    buffer_->next_sequence = 1U;
    std::snprintf(buffer_->session_id.data(), buffer_->session_id.size(), "%s", session_id);
}

void GuestLogBuffer::UpdateAppLifecycle(const char* app_id) {
    if (buffer_ == nullptr) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (app_id == nullptr || app_id[0] == '\0') {
        buffer_->active_app_id.fill('\0');
        return;
    }
    if (std::strcmp(buffer_->active_app_id.data(), app_id) == 0) {
        return;
    }
    buffer_->entries = {};
    buffer_->start = 0U;
    buffer_->count = 0U;
    buffer_->next_sequence = 1U;
    ++buffer_->session_generation;
    std::snprintf(buffer_->session_id.data(), buffer_->session_id.size(), "%016" PRIx64, buffer_->session_generation);
    std::snprintf(buffer_->active_app_id.data(), buffer_->active_app_id.size(), "%s", app_id);
}

void GuestLogBuffer::Write(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                           uint64_t timestamp_us) {
    if (buffer_ == nullptr || (bytes == nullptr && length != 0U)) {
        return;
    }
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
    std::snprintf(entry.app_id.data(), entry.app_id.size(), "%s", app_id != nullptr ? app_id : "");
    const size_t copied = std::min(length, entry.message.size() - 1U);
    if (copied != 0U) {
        std::memcpy(entry.message.data(), bytes, copied);
    }
    entry.message[copied] = '\0';
}

uint64_t GuestLogBuffer::NormalizeCursor(const char* session_id, uint64_t after_sequence) const {
    std::lock_guard lock(mutex_);
    return buffer_ != nullptr && session_id != nullptr && std::strcmp(session_id, buffer_->session_id.data()) == 0
               ? after_sequence
               : 0U;
}

GuestLogBuffer::Page GuestLogBuffer::ReadOne(const char* session_id, uint64_t after_sequence) const {
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
    for (size_t offset = 0U; offset < buffer_->count; ++offset) {
        const Entry& source = buffer_->entries[(buffer_->start + offset) % buffer_->entries.size()];
        if (source.sequence <= normalized_after) {
            continue;
        }
        page.entry.sequence = source.sequence;
        page.entry.timestamp_us = source.timestamp_us;
        page.entry.level = source.level;
        page.entry.app_id = source.app_id;
        page.entry.message = source.message;
        page.next_sequence = source.sequence;
        page.has_entry = true;
        page.has_more = offset + 1U < buffer_->count;
        break;
    }
    return page;
}

cJSON* GuestLogBuffer::CreatePayload(uint64_t after_sequence, size_t maximum_entries, uint64_t& next_cursor_out,
                                     bool& has_entries_out) const {
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
            if (buffer_->session_id[0] != '\0') {
                (void)cJSON_AddStringToObject(result, "appSessionId", buffer_->session_id.data());
            } else {
                (void)cJSON_AddNullToObject(result, "appSessionId");
            }
            if (buffer_->count != 0U) {
                const uint64_t oldest_sequence = buffer_->entries[buffer_->start].sequence;
                truncated = oldest_sequence > after_sequence && oldest_sequence - after_sequence > 1U;
            }
            for (size_t offset = 0U; offset < buffer_->count; ++offset) {
                const Entry& entry = buffer_->entries[(buffer_->start + offset) % buffer_->entries.size()];
                if (entry.sequence <= after_sequence) {
                    continue;
                }
                if (appended >= maximum_entries) {
                    has_more = true;
                    break;
                }
                const size_t entry_json_upper_bound =
                    (std::strlen(entry.app_id.data()) + std::strlen(entry.message.data())) * 6U + 256U;
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

}  // namespace micropixel::firmware::control
