#include "sdk/panic.hpp"

#include "runtime/service_binding.hpp"
#include "sdk/log.hpp"

using micropixel::runtime::RequireOk;

namespace {

uint32_t BoundedLength(const char* message) {
    uint32_t length = 0U;
    while (length < MICROPIXEL_ABI_MAX_LOG_BYTES && message[length] != '\0') {
        ++length;
    }
    return length;
}

const char* StatusName(int32_t status) {
    switch (status) {
        case MICROPIXEL_STATUS_INVALID_ARGUMENT:
            return "invalid_argument";
        case MICROPIXEL_STATUS_INVALID_MEMORY:
            return "invalid_memory";
        case MICROPIXEL_STATUS_UNSUPPORTED:
            return "unsupported";
        case MICROPIXEL_STATUS_RESOURCE_EXHAUSTED:
            return "resource_exhausted";
        case MICROPIXEL_STATUS_INTERNAL:
            return "internal";
        case MICROPIXEL_STATUS_NOT_FOUND:
            return "not_found";
        case MICROPIXEL_STATUS_PERMISSION_DENIED:
            return "permission_denied";
        case MICROPIXEL_STATUS_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        case MICROPIXEL_STATUS_RATE_LIMITED:
            return "rate_limited";
        case MICROPIXEL_STATUS_WOULD_BLOCK:
            return "would_block";
        case MICROPIXEL_STATUS_TIMEOUT:
            return "timeout";
        case MICROPIXEL_STATUS_CANCELLED:
            return "cancelled";
        case MICROPIXEL_STATUS_CLOSED:
            return "closed";
        case MICROPIXEL_STATUS_VERSION_MISMATCH:
            return "version_mismatch";
        case MICROPIXEL_STATUS_STALE_STATE:
            return "stale_state";
        default:
            return "internal";
    }
}

void DiagnosticLine(const char* message) {
    uint32_t length = BoundedLength(message);
    if (length < MICROPIXEL_ABI_MAX_LOG_BYTES) {
        (void)micropixel_log_write(MICROPIXEL_LOG_ERROR, reinterpret_cast<const uint8_t*>(message), length);
    }
}

// Bounded, allocation-free builder for the single-line panic report. The Host
// keeps the last line that starts with kPanicPrefix and attaches it to the
// session failure, so everything a developer needs must fit on this one line.
constexpr char kPanicPrefix[] = "panic: ";

class PanicLine final {
   public:
    void Append(const char* text) {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && length_ + 1U < sizeof(buffer_)) {
            buffer_[length_++] = *text++;
        }
        buffer_[length_] = '\0';
    }

    void AppendDecimal(int32_t value) {
        char digits[12];
        uint32_t count = 0U;
        uint32_t magnitude = value < 0 ? 0U - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
        do {
            digits[count++] = static_cast<char>('0' + magnitude % 10U);
            magnitude /= 10U;
        } while (magnitude != 0U);
        if (value < 0) {
            Append("-");
        }
        while (count != 0U && length_ + 1U < sizeof(buffer_)) {
            buffer_[length_++] = digits[--count];
        }
        buffer_[length_] = '\0';
    }

    [[nodiscard]] const char* c_str() const { return buffer_; }

   private:
    char buffer_[256]{};
    uint32_t length_{};
};
void WriteLog(uint32_t level, const char* message, const char* operation) {
    if (message == nullptr) {
        micropixel::runtime::Panic(operation, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t length = BoundedLength(message);
    if (length == MICROPIXEL_ABI_MAX_LOG_BYTES) {
        micropixel::runtime::Panic(operation, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    RequireOk(micropixel_log_write(level, reinterpret_cast<const uint8_t*>(message), length), operation);
}

}  // namespace

namespace micropixel {

static_assert(Log::kMaximumMessageBytes + 1U == MICROPIXEL_ABI_MAX_LOG_BYTES, "SDK/ABI log message limit drifted");

[[noreturn]] void Panic(const char* reason) {
    if (reason == nullptr || BoundedLength(reason) == MICROPIXEL_ABI_MAX_LOG_BYTES) {
        runtime::Panic("panic.reason", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    PanicLine line;
    line.Append(kPanicPrefix);
    line.Append(reason);
    DiagnosticLine(line.c_str());
    __builtin_trap();
}

void Log::Debug(const char* message) const { WriteLog(MICROPIXEL_LOG_DEBUG, message, "log.debug"); }

void Log::Info(const char* message) const { WriteLog(MICROPIXEL_LOG_INFO, message, "log.info"); }

void Log::Warning(const char* message) const { WriteLog(MICROPIXEL_LOG_WARNING, message, "log.warning"); }

void Log::Error(const char* message) const { WriteLog(MICROPIXEL_LOG_ERROR, message, "log.error"); }

}  // namespace micropixel

namespace micropixel::runtime {

[[noreturn]] void Panic(const char* operation, int32_t status) {
    // One line, e.g. "panic: graphics.info failed: buffer_too_small (status=-6)".
    PanicLine line;
    line.Append(kPanicPrefix);
    line.Append(operation);
    line.Append(" failed: ");
    line.Append(StatusName(status));
    line.Append(" (status=");
    line.AppendDecimal(status);
    line.Append(")");
    DiagnosticLine(line.c_str());
    __builtin_trap();
}

}  // namespace micropixel::runtime
