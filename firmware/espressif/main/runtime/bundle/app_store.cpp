#include "runtime/bundle/app_store.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "psa/crypto.h"
#include "runtime/bundle/bundle_format.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/bundlefs/bundlefs.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "app_store";
constexpr size_t kWriteChunkSize = 4096U;

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
                                                             const bundlefs_file_info_t& file_info) {
    micropixel_bundle_metadata_t metadata{};
    if (!micropixel_read_bundle_metadata(&file, &metadata) || metadata.bundle_size != file_info.size ||
        std::strcmp(reinterpret_cast<const char*>(metadata.app_id), file_info.name) != 0) {
        return std::unexpected(AppStoreError::kCatalogCorrupt);
    }
    InstalledApp app{};
    std::snprintf(app.app_id.data(), app.app_id.size(), "%s", reinterpret_cast<const char*>(metadata.app_id));
    std::snprintf(app.display_name.data(), app.display_name.size(), "%s",
                  reinterpret_cast<const char*>(metadata.display_name));
    app.bundle_size = metadata.bundle_size;
    app.content_id = file_info.content_id;
    app.file = file;
    return app;
}

std::expected<InstalledApp, AppStoreError> OpenInstalledApp(const char* app_id) {
    bundlefs_file_t file{};
    bundlefs_file_info_t file_info{};
    bundlefs_error_t error = bundlefs_open(app_id, &file);
    if (error == BUNDLEFS_OK) {
        error = bundlefs_get_file_info(&file, &file_info);
    }
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    return InstalledFromFile(file, file_info);
}

}  // namespace

std::expected<void, AppStoreError> LoadAppStoreCatalog(InstalledAppCatalog& catalog_out) {
    catalog_out = {};
    bundlefs_error_t error = bundlefs_mount();
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }

    std::array<bundlefs_file_info_t, kMaxInstalledApps> files{};
    uint32_t file_count = 0U;
    error = bundlefs_list(files.data(), files.size(), &file_count);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    bundlefs_store_info_t store_info{};
    error = bundlefs_get_store_info(&store_info);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }

    catalog_out.count = file_count;
    catalog_out.store_total_bytes = store_info.total_bytes;
    catalog_out.store_used_bytes = store_info.used_bytes;
    for (uint32_t index = 0U; index < file_count; ++index) {
        bundlefs_file_t file{};
        error = bundlefs_open(files[index].name, &file);
        if (error != BUNDLEFS_OK) {
            return std::unexpected(MapBundleFsError(error));
        }
        auto installed = InstalledFromFile(file, files[index]);
        if (!installed) {
            return std::unexpected(installed.error());
        }
        catalog_out.apps[index] = *installed;
    }
    return {};
}

std::expected<AppInstallResult, AppStoreError> InstallApp(const AppInstallRequest& request) {
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
        auto installed = OpenInstalledApp(request.expected_app_id);
        if (!installed) {
            return std::unexpected(installed.error());
        }
        ESP_LOGI(kTag, "App is already current: app=%s content=%08" PRIx32, request.expected_app_id,
                 installed->content_id);
        return AppInstallResult{.app = *installed, .changed = false};
    }
    if (error != BUNDLEFS_OK && error != BUNDLEFS_ERR_NOT_FOUND) {
        return std::unexpected(MapBundleFsError(error));
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
    micropixel_aot_package_t validation{};
    if (!micropixel_open_aot_package(&staged_file, &validation)) {
        abort_install();
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    micropixel_close_aot_package(&validation);

    micropixel_bundle_metadata_t metadata{};
    if (!micropixel_read_bundle_metadata(&staged_file, &metadata)) {
        abort_install();
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (std::strcmp(reinterpret_cast<const char*>(metadata.app_id), request.expected_app_id) != 0) {
        abort_install();
        return std::unexpected(AppStoreError::kAppIdMismatch);
    }

    error = bundlefs_commit(&writer, request.expected_sha256.data());
    if (error != BUNDLEFS_OK) {
        abort_install();
        return std::unexpected(MapBundleFsError(error));
    }

    auto installed = OpenInstalledApp(request.expected_app_id);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    ESP_LOGI(kTag, "committed App: app=%s bytes=%zu content=%08" PRIx32, request.expected_app_id, request.size,
             installed->content_id);
    return AppInstallResult{.app = *installed, .changed = true};
}

std::expected<void, AppStoreError> UninstallApp(const char* app_id) {
    if (!ValidAppId(app_id)) {
        return std::unexpected(AppStoreError::kNotFound);
    }
    const bundlefs_error_t error = bundlefs_remove(app_id);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    ESP_LOGI(kTag, "removed App: app=%s", app_id);
    return {};
}

}  // namespace micropixel::runtime
