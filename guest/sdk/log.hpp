#ifndef MICROPIXEL_SDK_LOG_HPP
#define MICROPIXEL_SDK_LOG_HPP

#include <stdint.h>

namespace micropixel {

class Application;
namespace ui {
class ImageButton;
class TextButton;
}

// Lightweight view of the logging service for this Guest. Copies refer to the
// same service and do not own a logging resource.
class Log final {
   public:
    // Maximum message payload, excluding the terminating NUL.
    static constexpr uint32_t kMaximumMessageBytes = 1023U;

    constexpr Log(const Log&) noexcept = default;
    constexpr Log& operator=(const Log&) noexcept = default;

    void Debug(const char* message) const;
    void Info(const char* message) const;
    void Warning(const char* message) const;
    void Error(const char* message) const;

   private:
    struct CapabilityToken {};
    explicit constexpr Log(CapabilityToken) noexcept {}
    friend class Application;
    friend class ui::ImageButton;
    friend class ui::TextButton;
};

}  // namespace micropixel

#endif
