#ifndef MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "runtime/bundle/aot_package.hpp"
#include "runtime/bundlefs/bundle_store.hpp"

namespace micropixel::runtime {

enum class AppStoreError : uint8_t {
    kUnavailable,
    kCatalogCorrupt,
    kInvalidPackage,
    kIncompatibleAotTarget,
    kHashMismatch,
    kAppIdMismatch,
    kCatalogFull,
    kNoSpace,
    kFlashWrite,
    kCommitFailed,
    kNotFound,
    kUntrustedComponent,
    kComponentActive,
};

struct AppInstallRequest final {
    const uint8_t* data{};
    size_t size{};
    const char* expected_app_id{};
    std::array<uint8_t, 32U> expected_sha256{};
    // Set only after a future trusted publisher signature verifier succeeds.
    // Current remote/local App install paths leave this false.
    bool trusted_component_signature{};
    const micropixel_app_environment_t* environment{};
    const char* expected_version{};
};

struct AppInstallResult final {
    InstalledApp app{};
    uint32_t package_type{MICROPIXEL_BUNDLE_PACKAGE_APP};
    bool changed{};
};

// Host-owned App Store policy over one or two Bundle stores. The system store
// (XIP NOR) keeps Components and factory Apps; the optional external store
// (board NAND or a removable card) receives downloaded Apps while it is
// mounted. An external store that is present but not ready (unformatted,
// foreign geometry, damaged) is reported through the catalog so the System UI
// can offer to format it; until then downloaded Apps fall back to the system
// store. Boards with one medium pass no external store.
class AppStore final {
   public:
    // The external store's state is determined by the first LoadCatalog().
    explicit AppStore(BundleStore& system_store, BundleStore* external_store = nullptr)
        : system_store_(system_store),
          external_store_(external_store),
          external_state_(external_store != nullptr ? ExternalStorageState::kUnavailable
                                                    : ExternalStorageState::kAbsent) {}
    AppStore(const AppStore&) = delete;
    AppStore& operator=(const AppStore&) = delete;

    [[nodiscard]] BundleStore& system_store() const { return system_store_; }  // NOLINT(readability-identifier-naming)
    // The store downloaded Apps are written to right now.
    [[nodiscard]] BundleStore& app_store() const {  // NOLINT(readability-identifier-naming)
        return ExternalReady() ? *external_store_ : system_store_;
    }
    [[nodiscard]] bool split() const { return ExternalReady(); }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ExternalStorageState external_state() const {   // NOLINT(readability-identifier-naming)
        return external_state_;
    }

    // Erases the external store's Catalog and mounts it empty. Every App on it
    // is lost; the caller confirms with the user and ensures no AppSession is
    // running. Fails with kUnavailable when the board has no external store.
    [[nodiscard]] std::expected<void, AppStoreError> FormatExternalStore();

    // InstalledAppCatalog contains up to fifty opaque store handles and is too
    // large to return by value on the bounded Host supervisor stack. The caller
    // owns its storage so production paths can keep it in PSRAM. Downloaded
    // Apps are listed before factory Apps; each store lists newest first.
    [[nodiscard]] std::expected<void, AppStoreError> LoadCatalog(InstalledAppCatalog& catalog_out,
                                                                 std::string_view effective_locale = "en");
    // Replacement retains the old Bundle until the new Catalog commits. Requires
    // enough free space for the new Bundle; failure leaves the old App installed.
    // The caller must ensure no AppSession is running.
    [[nodiscard]] std::expected<AppInstallResult, AppStoreError> Install(const AppInstallRequest& request,
                                                                         std::string_view effective_locale = "en");
    [[nodiscard]] std::expected<void, AppStoreError> UninstallApp(const char* app_id);
    [[nodiscard]] std::expected<void, AppStoreError> UninstallComponent(const char* component_id,
                                                                        std::string_view active_component_id = {});

   private:
    struct LocatedFile final {
        BundleStore* store{};
        bundlefs_file_t file{};
        bundlefs_file_info_t info{};
    };

    [[nodiscard]] bool ExternalReady() const {
        return external_store_ != nullptr && external_state_ == ExternalStorageState::kReady;
    }
    // Mounts the external store and records its state; true when it is ready.
    [[nodiscard]] bool RefreshExternalState();
    [[nodiscard]] std::array<BundleStore*, 2U> Stores() const;
    [[nodiscard]] std::expected<LocatedFile, AppStoreError> Locate(const char* name);
    [[nodiscard]] std::expected<micropixel_bundle_metadata_t, AppStoreError> ReadInstalledMetadata(
        const char* package_id, std::string_view effective_locale);
    [[nodiscard]] std::expected<InstalledApp, AppStoreError> OpenInstalledApp(const char* app_id,
                                                                              std::string_view effective_locale);
    [[nodiscard]] std::expected<void, AppStoreError> LoadStoreCatalog(BundleStore& store, AppStorage storage,
                                                                      InstalledAppCatalog& catalog_out,
                                                                      std::string_view effective_locale);

    BundleStore& system_store_;
    BundleStore* external_store_{};
    ExternalStorageState external_state_{ExternalStorageState::kAbsent};
};

}  // namespace micropixel::runtime

#endif
