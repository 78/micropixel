#ifndef MICROPIXEL_SDK_LOCALIZATION_HPP
#define MICROPIXEL_SDK_LOCALIZATION_HPP

namespace micropixel {

class Locale final {
   public:
    [[nodiscard]] constexpr const char* tag() const noexcept { return tag_; }

   private:
    char tag_[32]{'e', 'n', '\0'};
    friend class Localization;
};

class Localization final {
   public:
    [[nodiscard]] Locale CurrentLocale() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Localization(CapabilityToken) {}
    friend class Application;
};

}  // namespace micropixel

#endif
