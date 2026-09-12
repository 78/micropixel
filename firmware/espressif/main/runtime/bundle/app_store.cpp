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
#include "runtime/bundle/memory_bundle_source.h"
#include "runtime/bundlefs/bundle_store_source.hpp"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "app_store";
constexpr size_t kWriteChunkSize = 4096U;

constexpr uint32_t HostAotTargetMask() {
#if CONFIG_IDF_TARGET_ESP32S3
    return MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3;
#elif CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    return MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F;
#else
#error "MicroPixel App Store requires an explicit Host AOT target"
#endif
}

uint32_t Fnv1a32(const uint8_t* data, size_t size) {
    uint32_t value = 2166136261U;
    for (size_t index = 0U; index < size; ++index) {
        value ^= data[index];
        value *= 16777619U;
    }
    return value;
}

std::expected<void, AppStoreError> PreflightAotTarget(const AppInstallRequest& request,
                                                      const micropixel_bundle_header_t& header) {
    if (header.header_size != sizeof(header) || header.toc_offset != sizeof(header) || header.section_count == 0U ||
        header.section_count > MICROPIXEL_BUNDLE_MAX_SECTIONS ||
        header.section_count > (request.size - header.toc_offset) / sizeof(micropixel_bundle_section_t) ||
        header.framework_abi_version != MICROPIXEL_BUNDLE_FRAMEWORK_ABI_VERSION || header.reserved0 != 0U ||
        header.reserved1 != 0U || header.reserved2 != 0U || header.reserved3 != 0U || header.reserved4 != 0U) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    micropixel_bundle_header_t hash_header = header;
    const uint32_t expected_hash = hash_header.header_hash;
    hash_header.header_hash = 0U;
    if (Fnv1a32(reinterpret_cast<const uint8_t*>(&hash_header), sizeof(hash_header)) != expected_hash) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }

    uint32_t aot_sections = 0U;
    uint32_t aot_target_mask = MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE;
    for (uint32_t index = 0U; index < header.section_count; ++index) {
        micropixel_bundle_section_t section{};
        std::memcpy(&section, request.data + header.toc_offset + index * sizeof(section), sizeof(section));
        if (section.kind == MICROPIXEL_BUNDLE_SECTION_AOT) {
            ++aot_sections;
            aot_target_mask = section.reserved0;
        }
    }
    const bool known_target = aot_target_mask == MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE ||
                              aot_target_mask == MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F ||
                              aot_target_mask == MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3;
    if (aot_sections > 1U || !known_target) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (aot_sections == 1U &&
        (aot_target_mask == MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE || aot_target_mask != HostAotTargetMask())) {
        return std::unexpected(AppStoreError::kIncompatibleAotTarget);
    }
    return {};
}

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
        case BUNDLEFS_ERR_NOT_FORMATTED:
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

// Every store file handed out is wrapped once here so the rest of the Host
// only sees the storage-agnostic Bundle source.
std::expected<micropixel_bundle_source_t, AppStoreError> SourceFromFile(BundleStore& store,
                                                                        const bundlefs_file_t& file) {
    micropixel_bundle_source_t source{};
    if (!MakeBundleSource(store, file, source)) {
        return std::unexpected(AppStoreError::kUnavailable);
    }
    return source;
}

std::expected<InstalledApp, AppStoreError> InstalledFromFile(BundleStore& store, const bundlefs_file_t& file,
                                                             const bundlefs_file_info_t& file_info,
                                                             std::string_view effective_locale) {
    auto source = SourceFromFile(store, file);
    if (!source) {
        return std::unexpected(source.error());
    }
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_read_bundle_metadata_for_locale(&*source, locale.data(), &metadata) ||
        metadata.bundle_size != file_info.size ||
        std::strcmp(reinterpret_cast<const char*>(metadata.app_id), file_info.name) != 0) {
        return std::unexpected(AppStoreError::kCatalogCorrupt);
    }
    InstalledApp app{};
    std::snprintf(app.app_id.data(), app.app_id.size(), "%s", reinterpret_cast<const char*>(metadata.app_id));
    std::snprintf(app.display_name.data(), app.display_name.size(), "%s",
                  reinterpret_cast<const char*>(metadata.display_name));
    app.display_profile = metadata.display_profile;
    std::memcpy(app.version.data(), metadata.package_version, app.version.size());
    app.bundle_size = metadata.bundle_size;
    app.content_id = file_info.content_id;
    std::copy_n(file_info.sha256, app.sha256.size(), app.sha256.begin());
    app.source = *source;
    return app;
}

const char* StoreRole(const AppStore& store, const BundleStore& target) {
    return &target == &store.system_store() ? "system store" : "external store";
}

ExternalStorageState ExternalStateFromMount(bundlefs_error_t error) {
    switch (error) {
        case BUNDLEFS_OK:
            return ExternalStorageState::kReady;
        case BUNDLEFS_ERR_NOT_FORMATTED:
            return ExternalStorageState::kNotFormatted;
        case BUNDLEFS_ERR_UNSUPPORTED_FORMAT:
            return ExternalStorageState::kUnsupportedFormat;
        case BUNDLEFS_ERR_CORRUPT:
            return ExternalStorageState::kCorrupt;
        default:
            return ExternalStorageState::kUnavailable;
    }
}

}  // namespace

bool AppStore::RefreshExternalState() {
    if (external_store_ == nullptr) {
        external_state_ = ExternalStorageState::kAbsent;
        return false;
    }
    const bundlefs_error_t error = external_store_->Mount();
    const ExternalStorageState state = ExternalStateFromMount(error);
    if (state != external_state_) {
        ESP_LOGI(kTag, "external store state: %u (BundleFS error %d)", static_cast<unsigned>(state),
                 static_cast<int>(error));
    }
    external_state_ = state;
    return state == ExternalStorageState::kReady;
}

std::expected<void, AppStoreError> AppStore::FormatExternalStore() {
    if (external_store_ == nullptr) {
        return std::unexpected(AppStoreError::kUnavailable);
    }
    ESP_LOGW(kTag, "formatting the external store; every App on it is erased");
    const bundlefs_error_t error = external_store_->Format();
    if (error != BUNDLEFS_OK) {
        ESP_LOGE(kTag, "external store format failed: BundleFS error %d", static_cast<int>(error));
        (void)RefreshExternalState();
        return std::unexpected(MapBundleFsError(error));
    }
    if (!RefreshExternalState()) {
        return std::unexpected(AppStoreError::kCatalogCorrupt);
    }
    return {};
}

std::array<BundleStore*, 2U> AppStore::Stores() const {
    return {ExternalReady() ? external_store_ : &system_store_, ExternalReady() ? &system_store_ : nullptr};
}

std::expected<AppStore::LocatedFile, AppStoreError> AppStore::Locate(const char* name) {
    for (BundleStore* store : Stores()) {
        if (store == nullptr) {
            continue;
        }
        LocatedFile located{};
        located.store = store;
        bundlefs_error_t error = store->Open(name, located.file);
        if (error == BUNDLEFS_ERR_NOT_FOUND) {
            continue;
        }
        if (error == BUNDLEFS_OK) {
            error = store->GetFileInfo(located.file, located.info);
        }
        if (error == BUNDLEFS_OK) {
            error = store->GetFileSha256(name, located.info.sha256);
        }
        if (error != BUNDLEFS_OK) {
            return std::unexpected(MapBundleFsError(error));
        }
        return located;
    }
    return std::unexpected(AppStoreError::kNotFound);
}

std::expected<micropixel_bundle_metadata_t, AppStoreError> AppStore::ReadInstalledMetadata(
    const char* package_id, std::string_view effective_locale) {
    auto located = Locate(package_id);
    if (!located) {
        return std::unexpected(located.error());
    }
    auto source = SourceFromFile(*located->store, located->file);
    if (!source) {
        return std::unexpected(source.error());
    }
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_read_bundle_metadata_for_locale(&*source, locale.data(), &metadata) ||
        std::strcmp(reinterpret_cast<const char*>(metadata.app_id), package_id) != 0) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    return metadata;
}

std::expected<InstalledApp, AppStoreError> AppStore::OpenInstalledApp(const char* app_id,
                                                                      std::string_view effective_locale) {
    auto located = Locate(app_id);
    if (!located) {
        return std::unexpected(located.error());
    }
    auto installed = InstalledFromFile(*located->store, located->file, located->info, effective_locale);
    if (installed) {
        installed->storage = located->store == &system_store_ ? AppStorage::kSystem : AppStorage::kExternal;
    }
    return installed;
}

std::expected<void, AppStoreError> AppStore::LoadStoreCatalog(BundleStore& store, AppStorage storage,
                                                              InstalledAppCatalog& catalog_out,
                                                              std::string_view effective_locale) {
    bundlefs_error_t error = store.Mount();
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
    error = store.List(files->data(), files->size(), file_count);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    bundlefs_store_info_t store_info{};
    error = store.GetStoreInfo(store_info);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }

    catalog_out.store_total_bytes += store_info.total_bytes;
    catalog_out.store_used_bytes += store_info.used_bytes;
    StorageUsage& usage = storage == AppStorage::kExternal ? catalog_out.external_storage : catalog_out.system_storage;
    usage.total_bytes = store_info.total_bytes;
    usage.used_bytes = store_info.used_bytes;
    for (uint32_t index = 0U; index < file_count; ++index) {
        bundlefs_file_t file{};
        error = store.Open((*files)[index].name, file);
        if (error != BUNDLEFS_OK) {
            return std::unexpected(MapBundleFsError(error));
        }
        auto source = SourceFromFile(store, file);
        if (!source) {
            return std::unexpected(source.error());
        }
        micropixel_bundle_metadata_t metadata{};
        const auto locale = LocaleBuffer(effective_locale);
        if (!micropixel_read_bundle_metadata_for_locale(&*source, locale.data(), &metadata) ||
            metadata.bundle_size != (*files)[index].size ||
            std::strcmp(reinterpret_cast<const char*>(metadata.app_id), (*files)[index].name) != 0) {
            return std::unexpected(AppStoreError::kCatalogCorrupt);
        }
        if (metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
            micropixel_bundle_metadata_t validated{};
            if (!micropixel_validate_component_package(&*source, &validated)) {
                return std::unexpected(AppStoreError::kCatalogCorrupt);
            }
            ++catalog_out.component_count;
            continue;
        }
        // The same AppId never lives in two stores after a successful install;
        // a stale duplicate left by an interrupted migration hides behind the
        // newer store rather than appearing twice in the Hall.
        bool duplicate = false;
        for (uint32_t existing = 0U; existing < catalog_out.count && !duplicate; ++existing) {
            duplicate = std::strcmp(catalog_out.apps[existing].app_id.data(), (*files)[index].name) == 0;
        }
        if (duplicate) {
            ESP_LOGW(kTag, "ignoring duplicate App in %s: app=%s", StoreRole(*this, store), (*files)[index].name);
            continue;
        }
        auto installed = InstalledFromFile(store, file, (*files)[index], effective_locale);
        if (!installed || catalog_out.count >= catalog_out.apps.size()) {
            return std::unexpected(installed ? AppStoreError::kCatalogFull : installed.error());
        }
        installed->storage = storage;
        catalog_out.apps[catalog_out.count++] = *installed;
    }
    return {};
}

std::expected<void, AppStoreError> AppStore::LoadCatalog(InstalledAppCatalog& catalog_out,
                                                         std::string_view effective_locale) {
    catalog_out.Reset();
    // The external store is listed first so its Apps lead the Hall. A medium
    // that is not ready (unformatted, foreign geometry, damaged) must not hide
    // the system store: Components and factory Apps stay usable without it,
    // and the state is reported so the System UI can offer to format it.
    if (RefreshExternalState()) {
        auto loaded = LoadStoreCatalog(*external_store_, AppStorage::kExternal, catalog_out, effective_locale);
        if (!loaded) {
            ESP_LOGE(kTag, "external store catalog unusable (error %d); listing the system store only",
                     static_cast<int>(loaded.error()));
            external_state_ = ExternalStorageState::kCorrupt;
            catalog_out.Reset();
        }
    }
    catalog_out.external_state = external_state_;
    return LoadStoreCatalog(system_store_, AppStorage::kSystem, catalog_out, effective_locale);
}

std::expected<AppInstallResult, AppStoreError> AppStore::Install(const AppInstallRequest& request,
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
    auto target_preflight = PreflightAotTarget(request, header);
    if (!target_preflight) {
        return std::unexpected(target_preflight.error());
    }

    // The payload is already in RAM, so its package type decides the target
    // store before any flash is touched: Components always live in the system
    // store, Apps go to the App store.
    micropixel_bundle_source_t payload{};
    micropixel_bundle_metadata_t metadata{};
    const auto locale = LocaleBuffer(effective_locale);
    if (!micropixel_memory_bundle_source(request.data, static_cast<uint32_t>(request.size), &payload) ||
        !micropixel_read_bundle_metadata_for_locale(&payload, locale.data(), &metadata)) {
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (std::strcmp(reinterpret_cast<const char*>(metadata.app_id), request.expected_app_id) != 0) {
        return std::unexpected(AppStoreError::kAppIdMismatch);
    }
    const bool component = metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
    if (component && !request.trusted_component_signature) {
        return std::unexpected(AppStoreError::kUntrustedComponent);
    }
    BundleStore& target = component ? system_store_ : app_store();
    if (!component && external_store_ != nullptr && !ExternalReady()) {
        ESP_LOGW(kTag, "external store is not ready (state %u); installing App to the system store",
                 static_cast<unsigned>(external_state_));
    }

    auto existing = Locate(request.expected_app_id);
    if (!existing && existing.error() != AppStoreError::kNotFound) {
        return std::unexpected(existing.error());
    }
    if (existing && std::equal(existing->info.sha256, existing->info.sha256 + BUNDLEFS_SHA256_SIZE,
                               request.expected_sha256.begin())) {
        auto installed_metadata = ReadInstalledMetadata(request.expected_app_id, effective_locale);
        if (!installed_metadata) {
            return std::unexpected(installed_metadata.error());
        }
        if (installed_metadata->package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
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

    bundlefs_store_info_t store_info{};
    if (target.GetStoreInfo(store_info) != BUNDLEFS_OK || store_info.data_block_size == 0U) {
        return std::unexpected(AppStoreError::kUnavailable);
    }
    // Keep the committed file alive until the replacement transaction commits.
    if (request.size + static_cast<uint64_t>(store_info.data_block_size) > store_info.free_bytes) {
        return std::unexpected(AppStoreError::kNoSpace);
    }

    bundlefs_writer_t writer{};
    bundlefs_error_t error = target.BeginReplace(request.expected_app_id, static_cast<uint32_t>(request.size), writer);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    const auto abort_install = [&target, &writer]() { target.Abort(writer); };

    for (size_t consumed = 0U; consumed < request.size; consumed += kWriteChunkSize) {
        const size_t chunk = std::min(kWriteChunkSize, request.size - consumed);
        error = target.Write(writer, request.data + consumed, static_cast<uint32_t>(chunk));
        if (error != BUNDLEFS_OK) {
            abort_install();
            return std::unexpected(MapBundleFsError(error));
        }
    }

    // Validate the bytes as they now sit in the target store, not the RAM copy.
    bundlefs_file_t staged_file{};
    error = target.OpenStaged(writer, staged_file);
    if (error != BUNDLEFS_OK) {
        abort_install();
        return std::unexpected(MapBundleFsError(error));
    }
    micropixel_bundle_source_t staged{};
    if (!MakeBundleSource(target, staged_file, staged)) {
        abort_install();
        return std::unexpected(AppStoreError::kUnavailable);
    }
    micropixel_bundle_metadata_t staged_metadata{};
    if (!micropixel_read_bundle_metadata_for_locale(&staged, locale.data(), &staged_metadata) ||
        staged_metadata.package_type != metadata.package_type) {
        abort_install();
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (std::strcmp(reinterpret_cast<const char*>(staged_metadata.app_id), request.expected_app_id) != 0) {
        abort_install();
        return std::unexpected(AppStoreError::kAppIdMismatch);
    }
    if (!micropixel_app_runtime_compatible(&staged_metadata.requirements, request.environment) ||
        (request.expected_version != nullptr &&
         std::strcmp(reinterpret_cast<const char*>(staged_metadata.package_version), request.expected_version) != 0)) {
        abort_install();
        return std::unexpected(AppStoreError::kInvalidPackage);
    }
    if (component) {
        micropixel_bundle_metadata_t validated{};
        if (!micropixel_validate_component_package(&staged, &validated)) {
            abort_install();
            return std::unexpected(AppStoreError::kInvalidPackage);
        }
    } else {
        // Streams every section hash from the staged file; nothing but the
        // AOT payload is ever held in RAM at once.
        if (!micropixel_validate_app_package(&staged, nullptr)) {
            abort_install();
            return std::unexpected(AppStoreError::kInvalidPackage);
        }
    }

    error = target.Commit(writer, request.expected_sha256.data());
    if (error != BUNDLEFS_OK) {
        abort_install();
        return std::unexpected(MapBundleFsError(error));
    }

    // An older copy in the other store (for example an App installed before
    // the board gained its NAND App store) is retired only after the new one
    // is committed, so failure never leaves the App missing.
    if (existing && existing->store != &target) {
        const bundlefs_error_t retire_error = existing->store->Remove(request.expected_app_id);
        if (retire_error != BUNDLEFS_OK) {
            ESP_LOGW(kTag, "stale copy remains in %s: app=%s error=%u", StoreRole(*this, *existing->store),
                     request.expected_app_id, static_cast<unsigned>(retire_error));
        } else {
            ESP_LOGI(kTag, "retired stale copy from %s: app=%s", StoreRole(*this, *existing->store),
                     request.expected_app_id);
        }
    }

    if (component) {
        ESP_LOGI(kTag, "committed Component to %s: id=%s bytes=%zu", StoreRole(*this, target), request.expected_app_id,
                 request.size);
        AppInstallResult result{};
        result.package_type = MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
        result.changed = true;
        return result;
    }
    auto installed = OpenInstalledApp(request.expected_app_id, effective_locale);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    ESP_LOGI(kTag, "committed App to %s: app=%s bytes=%zu content=%08" PRIx32, StoreRole(*this, target),
             request.expected_app_id, request.size, installed->content_id);
    return AppInstallResult{.app = *installed, .package_type = MICROPIXEL_BUNDLE_PACKAGE_APP, .changed = true};
}

std::expected<void, AppStoreError> AppStore::UninstallApp(const char* app_id) {
    if (!ValidAppId(app_id)) {
        return std::unexpected(AppStoreError::kNotFound);
    }
    auto metadata = ReadInstalledMetadata(app_id, "en");
    if (!metadata || metadata->package_type != MICROPIXEL_BUNDLE_PACKAGE_APP) {
        return std::unexpected(metadata ? AppStoreError::kNotFound : metadata.error());
    }
    auto located = Locate(app_id);
    if (!located) {
        return std::unexpected(located.error());
    }
    const bundlefs_error_t error = located->store->Remove(app_id);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    ESP_LOGI(kTag, "removed App from %s: app=%s", StoreRole(*this, *located->store), app_id);
    return {};
}

std::expected<void, AppStoreError> AppStore::UninstallComponent(const char* component_id,
                                                                std::string_view active_component_id) {
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
    auto located = Locate(component_id);
    if (!located) {
        return std::unexpected(located.error());
    }
    const bundlefs_error_t error = located->store->Remove(component_id);
    if (error != BUNDLEFS_OK) {
        return std::unexpected(MapBundleFsError(error));
    }
    ESP_LOGI(kTag, "removed Component from %s: id=%s", StoreRole(*this, *located->store), component_id);
    return {};
}

}  // namespace micropixel::runtime
