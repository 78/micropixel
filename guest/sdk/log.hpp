#ifndef MICROPIXEL_SDK_LOG_HPP
#define MICROPIXEL_SDK_LOG_HPP

namespace micropixel {

class Application;

// Lightweight view of the logging service for this Guest. Copies refer to the
// same service and do not own a logging resource.
class Log final {
   public:
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
};

}  // namespace micropixel

#endif
