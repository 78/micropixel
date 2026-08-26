#ifndef MICROPIXEL_SDK_ERROR_HPP
#define MICROPIXEL_SDK_ERROR_HPP

#include <stdint.h>

namespace micropixel {

enum class ErrorCode : int32_t {
    kInvalidArgument,
    kInvalidState,
    kUnsupported,
    kResourceExhausted,
    kPermissionDenied,
    kNotFound,
    kCancelled,
    kBufferTooSmall,
    kRateLimited,
    kInternal,
    kWouldBlock,
};

[[nodiscard]] constexpr const char* ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kInvalidArgument:
            return "invalid_argument";
        case ErrorCode::kInvalidState:
            return "invalid_state";
        case ErrorCode::kUnsupported:
            return "unsupported";
        case ErrorCode::kResourceExhausted:
            return "resource_exhausted";
        case ErrorCode::kPermissionDenied:
            return "permission_denied";
        case ErrorCode::kNotFound:
            return "not_found";
        case ErrorCode::kCancelled:
            return "cancelled";
        case ErrorCode::kBufferTooSmall:
            return "buffer_too_small";
        case ErrorCode::kRateLimited:
            return "rate_limited";
        case ErrorCode::kInternal:
            return "internal";
        case ErrorCode::kWouldBlock:
            return "would_block";
    }
    return "unknown";
}

class Error final {
   public:
    explicit constexpr Error(ErrorCode code) : code_(code) {}
    [[nodiscard]] constexpr ErrorCode code() const { return code_; }
    [[nodiscard]] constexpr const char* name() const noexcept { return ErrorCodeName(code_); }

    friend constexpr bool operator==(Error, Error) = default;

   private:
    ErrorCode code_;
};

}  // namespace micropixel

#endif
