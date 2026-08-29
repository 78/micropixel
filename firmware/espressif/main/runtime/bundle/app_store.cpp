#include "runtime/bundle/app_store.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "runtime/bundle/bundle_format.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/bundlefs/bundlefs.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "app_store";
constexpr size_t kWriteChunkSize = 4096U;

template <typename T>
struct HeapCapsObjectDeleter final {
    void operator()(T* value) const {
        if (value != nullptr) {
            value->~T();
            heap_caps_free(value);
        }
    }
};

template <typename T>
using HeapCapsObjectPtr = std::unique_ptr<T, HeapCapsObjectDeleter<T>>;

template <typename T>
HeapCapsObjectPtr<T> MakePsramObject() {
    void* memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return {};
    }
    return HeapCapsObjectPtr<T>(new (memory) T{});
}

std::array<char, MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH + 1U> LocaleBuffer(std::string_view locale) {
    if (locale.empty() || locale.size() > MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH) {
        locale = "en";
    }
    std::array<char, MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH + 1U> result{};
    std::copy(locale.begin(), locale.end(), result.begin());
    return result;
}

bool ValidAppId(const char* app_id) {
    if (app_id == nullptr) {
        return false;
    }
    const size_t length = ::strnlen(app_id, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U);
    if (length == 0U || length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char byte = app_id[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
              byte == '_' || byte == '-' || byte == '.')) {
            return false;
        }
    }
    return true;
}

AppStoreError MapBundleFsError(bundlefs_error_t error) {
    switch (error) {
        case BUNDLEFS_ERR_CORRUPT:
        case BUNDLEFS_ERR_UNSUPPORTED_FORMAT:
            return AppStoreError::kCatalogCorrupt;
        case BUNDLEFS_ERR_TOO_MANY_FILES:
            return AppStoreError::kCatalogFull;
        case BUNDLEFS_ERR_NO_SPACE:
            return AppStoreError::kNoSpace;
        case BUNDLEFS_ERR_HASH_MISMATCH:
            return AppStoreError::kHashMismatch;
        case BUNDLEFS_ERR_NOT_FOUND:
            return AppStoreError::kNotFound;
        case BUNDLEFS_ERR_IO:
            return AppStoreError::kFlashWrite;
        case BUNDLEFS_ERR_CONFLICT:
        case BUNDLEFS_ERR_COMMIT:
        case BUNDLEFS_ERR_BUSY:
            return AppStoreError::kCommitFailed;
        case BUNDLEFS_OK:
        case BUNDLEFS_ERR_UNAVAILABLE:
        case BUNDLEFS_ERR_INVALID_ARGUMENT:
        case BUNDLEFS_ERR_EXISTS:
            return AppStoreError::kUnavailable;
    }
    return AppStoreError::kUnavailable;
}

std::expected<std::array<uint8_t, BUNDLEFS_SHA256_SIZE>, AppStoreError> Sha256Memory(const uint8_t* data, size_t size) {
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    size_t digest_size = 0U;
    if (data == nullptr || psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, data, size, digest.data(), digest.size(), &digest_size) != PSA_SUCCESS ||
        digest_size != digest.size()) {
        return std::unexpected(AppStoreError::kUnavailable);
    }
    return digest;
}

std::expected<InstalledApp, AppStoreError> InstalledFromFile(const bundlefs_file_t& file,
                                                             const bundlefs_file_info_t& file_info,
                                                             std::string_view effective_locale) {
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_read_bundle_metadata_for_locale(&file, locale.data(), &metadata) ||
        metadata.bundle_size != file_info.size ||
        std::strcmp(reinterpret_cast<const char*>(metadata.app_id), file_info.name) != 0) {
        return std::unexpected(AppStoreError::kCatalogCorrupt);
    }
    InstalledApp app{};
    std::snprintf(app.app_id.data(), app.app_id.size(), "%s", reinterpret_cast<const char*>(metadata.app_id));
    std::snprintf(app.display_name.data(), app.display_name.size(), "%s",
                  reinterpret_cast<const char*>(metadata.display_name));
    app.display_profile = metadata.display_profile;
    app.bundle_size = metadata.bundle_size;
    app.content_id = file_info.content_id;
    app.file = file;
    return app;
}

std::expected<InstalledApp, AppStoreError> OpenInstalledApp(const char* app_id, std::string_view effective_locale) {
    bundlefs_file_t file{};
    bundlefs_file_info_t file_info{};
    bundlefs_error_t error = bundlefs_open(app_id, &file);
    if (error == BUNDLEFS_OK) {
        error = bundlefs_get_file_info(&file, &file_info);
    }
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    return InstalledFromFile(file, file_info, effective_locale);
}

std::expected<micropixel_bundle_metadata_t, AppStoreError> ReadInstalledMetadata(const char* package_id,
                                                                                 std::string_view effective_locale) {
    bundlefs_file_t file{};
    if (bundlefs_open(package_id, &file) != BUNDLEFS_OK) {
        return std::unexpected(AppStoreError::kNotFound);
    }
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_read_bundle_metadata_for_locale(&file, locale.data(), &metadata) ||
        std::strcmp(reinterpret_cast<const char*>(metadata.app_id), package_id) != 0) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    return metadata;
}

}  // namespace

std::expected<void, AppStoreError> LoadAppStoreCatalog(InstalledAppCatalog& catalog_out,
                                                       std::string_view effective_locale) {
    catalog_out = {};
    bundlefs_error_t error = bundlefs_mount();
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }

    using FileList = std::array<bundlefs_file_info_t, kMaxInstalledApps>;
    auto files = MakePsramObject<FileList>();
    if (files == nullptr) {
        ESP_LOGE(kTag, "App catalog file-list workspace requires %zu bytes of PSRAM", sizeof(FileList));
        return std::unexpected(AppStoreError::kUnavailable);
    }
    uint32_t file_count = 0U;
    error = bundlefs_list(files->data(), files->size(), &file_count);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    bundlefs_store_info_t store_info{};
    error = bundlefs_get_store_info(&store_info);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }

    catalog_out.store_total_bytes = store_info.total_bytes;
    catalog_out.store_used_bytes = store_info.used_bytes;
    for (uint32_t index = 0U; index < file_count; ++index) {
        bundlefs_file_t file{};
        error = bundlefs_open((*files)[index].name, &file);
        if (error != BUNDLEFS_OK) {
            return std::unexpected(MapBundleFsError(error));
        }
        micropixel_bundle_metadata_t metadata{};
        const auto locale = LocaleBuffer(effective_locale);
        if (!micropixel_read_bundle_metadata_for_locale(&file, locale.data(), &metadata) ||
            metadata.bundle_size != (*files)[index].size ||
            std::strcmp(reinterpret_cast<const char*>(metadata.app_id), (*files)[index].name) != 0) {
            return std::unexpected(AppStoreError::kCatalogCorrupt);
        }
        if (metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
            micropixel_bundle_metadata_t validated{};
            if (!micropixel_validate_component_package(&file, &validated)) {
                return std::unexpected(AppStoreError::kCatalogCorrupt);
            }
            ++catalog_out.component_count;
            continue;
        }
        auto installed = InstalledFromFile(file, (*files)[index], effective_locale);
        if (!installed || catalog_out.count >= catalog_out.apps.size()) {
            return std::unexpected(installed ? AppStoreError::kCatalogFull : installed.error());
        }
        catalog_out.apps[catalog_out.count++] = *installed;
    }
    return {};
}

std::expected<AppInstallResult, AppStoreError> InstallApp(const AppInstallRequest& request,
                                                          std::string_view effective_locale) {
    if (request.data == nullptr || request.size < sizeof(micropixel_bundle_header_t) || request.size > UINT32_MAX ||
        (request.size % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT) != 0U || !ValidAppId(request.expected_app_id)) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    auto actual_sha256 = Sha256Memory(request.data, request.size);
    if (!actual_sha256) {
        return std::unexpected(actual_sha256.error());
    }
    if (*actual_sha256 != request.expected_sha256) {
        return std::unexpected(AppStoreError::kHashMismatch);
    }

    micropixel_bundle_header_t header{};
    std::memcpy(&header, request.data, sizeof(header));
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> header_app_id{};
    if (std::memcmp(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MICROPIXEL_BUNDLE_VERSION || header.bundle_size != request.size ||
        header.app_id_length == 0U || header.app_id_length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    std::memcpy(header_app_id.data(), header.app_id, header.app_id_length);
    if (std::strcmp(header_app_id.data(), request.expected_app_id) != 0) {
        return std::unexpected(AppStoreError::kAppIdMismatch);
    }

    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> installed_sha256{};
    bundlefs_error_t error = bundlefs_get_file_sha256(request.expected_app_id, installed_sha256.data());
    if (error == BUNDLEFS_OK && installed_sha256 == request.expected_sha256) {
        auto metadata = ReadInstalledMetadata(request.expected_app_id, effective_locale);
        if (!metadata) {
            return std::unexpected(metadata.error());
        }
        if (metadata->package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
            if (!request.trusted_component_signature) {
                return std::unexpected(AppStoreError::kUntrustedComponent);
            }
            AppInstallResult result{};
            result.package_type = MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
            result.changed = false;
            return result;
        }
        auto installed = OpenInstalledApp(request.expected_app_id, effective_locale);
        if (!installed) {
            return std::unexpected(installed.error());
        }
        ESP_LOGI(kTag, "App is already current: app=%s content=%08" PRIx32, request.expected_app_id,
                 installed->content_id);
        return AppInstallResult{.app = *installed, .package_type = MICROPIXEL_BUNDLE_PACKAGE_APP, .changed = false};
    }
    if (error != BUNDLEFS_OK && error != BUNDLEFS_ERR_NOT_FOUND) {
        return std::unexpected(MapBundleFsError(error));
    }

    bundlefs_store_info_t store_info{};
    if (bundlefs_get_store_info(&store_info) != BUNDLEFS_OK) {
        return std::unexpected(AppStoreError::kUnavailable);
    }
    const uint64_t update_peak = request.size + store_info.data_block_size;
    if (store_info.data_block_size == 0U || update_peak > store_info.free_bytes) {
        return std::unexpected(AppStoreError::kNoSpace);
    }

    bundlefs_writer_t writer{};
    error = bundlefs_begin_replace(request.expected_app_id, static_cast<uint32_t>(request.size), &writer);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    const auto abort_install = [&writer]() { bundlefs_abort(&writer); };

    for (size_t consumed = 0U; consumed < request.size; consumed += kWriteChunkSize) {
        const size_t chunk = std::min(kWriteChunkSize, request.size - consumed);
        error = bundlefs_write(&writer, request.data + consumed, static_cast<uint32_t>(chunk));
        if (error != BUNDLEFS_OK) {
            abort_install();
            return std::unexpected(MapBundleFsError(error));
        }
    }

    bundlefs_file_t staged_file{};
    error = bundlefs_open_staged(&writer, &staged_file);
    if (error != BUNDLEFS_OK) {
        abort_install();
        return std::unexpected(MapBundleFsError(error));
    }
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_read_bundle_metadata_for_locale(&staged_file, locale.data(), &metadata)) {
        abort_install();
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (std::strcmp(reinterpret_cast<const char*>(metadata.app_id), request.expected_app_id) != 0) {
        abort_install();
        return std::unexpected(AppStoreError::kAppIdMismatch);
    }
    if (metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
        micropixel_bundle_metadata_t validated{};
        if (!request.trusted_component_signature) {
            abort_install();
            return std::unexpected(AppStoreError::kUntrustedComponent);
        }
        if (!micropixel_validate_component_package(&staged_file, &validated)) {
            abort_install();
            return std::unexpected(AppStoreError::kInvalidPackage);
        }
    } else {
        micropixel_aot_package_t validation{};
        if (!micropixel_open_aot_package(&staged_file, &validation)) {
            abort_install();
            return std::unexpected(AppStoreError::kInvalidPackage);
        }
        micropixel_close_aot_package(&validation);
    }

    error = bundlefs_commit(&writer, request.expected_sha256.data());
    if (error != BUNDLEFS_OK) {
        abort_install();
        return std::unexpected(MapBundleFsError(error));
    }

    if (metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
        ESP_LOGI(kTag, "committed Component: id=%s bytes=%zu", request.expected_app_id, request.size);
        AppInstallResult result{};
        result.package_type = MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
        result.changed = true;
        return result;
    }
    auto installed = OpenInstalledApp(request.expected_app_id, effective_locale);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    ESP_LOGI(kTag, "committed App: app=%s bytes=%zu content=%08" PRIx32, request.expected_app_id, request.size,
             installed->content_id);
    return AppInstallResult{.app = *installed, .package_type = MICROPIXEL_BUNDLE_PACKAGE_APP, .changed = true};
}

std::expected<void, AppStoreError> UninstallApp(const char* app_id) {
    if (!ValidAppId(app_id)) {
        return std::unexpected(AppStoreError::kNotFound);
    }
    auto metadata = ReadInstalledMetadata(app_id, "en");
    if (!metadata || metadata->package_type != MICROPIXEL_BUNDLE_PACKAGE_APP) {
        return std::unexpected(metadata ? AppStoreError::kNotFound : metadata.error());
    }
    const bundlefs_error_t error = bundlefs_remove(app_id);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    ESP_LOGI(kTag, "removed App: app=%s", app_id);
    return {};
}

std::expected<void, AppStoreError> UninstallComponent(const char* component_id, std::string_view active_component_id) {
    if (!ValidAppId(component_id)) {
        return std::unexpected(AppStoreError::kNotFound);
    }
    if (!active_component_id.empty() && active_component_id == component_id) {
        return std::unexpected(AppStoreError::kComponentActive);
    }
    auto metadata = ReadInstalledMetadata(component_id, "en");
    if (!metadata || metadata->package_type != MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
        return std::unexpected(metadata ? AppStoreError::kNotFound : metadata.error());
    }
    const bundlefs_error_t error = bundlefs_remove(component_id);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    ESP_LOGI(kTag, "removed Component: id=%s", component_id);
    return {};
}

}  // namespace micropixel::runtime
