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
};

class Error final {
   public:
    explicit constexpr Error(ErrorCode code) : code_(code) {}
    [[nodiscard]] constexpr ErrorCode code() const { return code_; }

    friend constexpr bool operator==(Error, Error) = default;

   private:
    ErrorCode code_;
};

}  // namespace micropixel

#endif
