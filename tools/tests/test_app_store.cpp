#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "psa/crypto.h"
#include "runtime/bundle/app_store.hpp"
#include "runtime/bundle/bundle_format.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/bundlefs/bundle_store.hpp"
#include "runtime/bundlefs/bundlefs_format.h"
#include "sdkconfig.h"

namespace {

constexpr uint32_t kMatchingTarget = CONFIG_IDF_TARGET_ESP32S3 ? MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3
                                                               : MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F;
constexpr uint32_t kOtherTarget = CONFIG_IDF_TARGET_ESP32S3 ? MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F
                                                            : MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3;

struct FakeFile final {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t content_id{};
};

uint32_t checks = 0U;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

std::array<uint8_t, BUNDLEFS_SHA256_SIZE> Hash(const std::vector<uint8_t>& data) {
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    size_t digest_size = 0U;
    Check(psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), digest.data(), digest.size(), &digest_size) ==
                  PSA_SUCCESS &&
              digest_size == digest.size(),
          "test hash must succeed");
    return digest;
}

// In-memory BundleStore with BundleFS semantics: newest file first, atomic
// replacement, one writer at a time. Handles carry the store identity so a
// handle from another instance is rejected like the real implementation does.
class FakeStore final : public micropixel::runtime::BundleStore {
   public:
    explicit FakeStore(uint32_t capacity_bytes, bool mappable = true, uint32_t tag = 1U)
        : capacity_bytes_(capacity_bytes), mappable_(mappable), tag_(tag) {}

    std::array<FakeFile, BUNDLEFS_MAX_FILES> files{};
    uint32_t file_count = 0U;
    FakeFile staged{};
    bool writer_active = false;
    bool fail_next_write = false;
    uint32_t removals = 0U;

    // Pre-populate a committed file, e.g. a factory App on the system store.
    void Seed(const char* name, const std::vector<uint8_t>& data) {
        for (uint32_t index = file_count; index > 0U; --index) {
            files[index] = std::move(files[index - 1U]);
        }
        files[0] = {.name = name, .data = data, .content_id = ++next_content_id_};
        ++file_count;
    }

    int FindFile(const char* name) const {
        for (uint32_t index = 0U; index < file_count; ++index) {
            if (files[index].name == name) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    bundlefs_error_t mount_error = BUNDLEFS_OK;
    bundlefs_error_t format_error = BUNDLEFS_OK;
    uint32_t formats = 0U;

    [[nodiscard]] bool mappable() const override { return mappable_; }
    [[nodiscard]] bundlefs_error_t Mount() override { return mount_error; }
    // A successful format always yields an empty, mountable store.
    [[nodiscard]] bundlefs_error_t Format() override {
        ++formats;
        if (format_error != BUNDLEFS_OK) {
            return format_error;
        }
        files = {};
        file_count = 0U;
        mount_error = BUNDLEFS_OK;
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t GetStoreInfo(bundlefs_store_info_t& info_out) override {
        uint32_t bundle_bytes = 0U;
        for (uint32_t index = 0U; index < file_count; ++index) {
            bundle_bytes += files[index].data.size();
        }
        const uint32_t used = MICROPIXEL_BUNDLEFS_METADATA_SIZE + bundle_bytes;
        info_out = {
            .data_block_size = MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT,
            .total_bytes = capacity_bytes_,
            .used_bytes = used,
            .free_bytes = capacity_bytes_ - used,
            .total_blocks = (capacity_bytes_ - MICROPIXEL_BUNDLEFS_METADATA_SIZE) / MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT,
            .used_blocks = bundle_bytes / MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT,
            .file_count = file_count,
        };
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t List(bundlefs_file_info_t* files_out, uint32_t capacity,
                                        uint32_t& count_out) override {
        if (files_out == nullptr || capacity < file_count) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        count_out = file_count;
        for (uint32_t index = 0U; index < file_count; ++index) {
            files_out[index] = {};
            std::snprintf(files_out[index].name, sizeof(files_out[index].name), "%s", files[index].name.c_str());
            files_out[index].size = files[index].data.size();
            files_out[index].content_id = files[index].content_id;
            const auto digest = Hash(files[index].data);
            std::copy(digest.begin(), digest.end(), files_out[index].sha256);
        }
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t GetFileSha256(const char* name, uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) override {
        const int index = name == nullptr ? -1 : FindFile(name);
        if (index < 0) {
            return BUNDLEFS_ERR_NOT_FOUND;
        }
        if (sha256_out == nullptr) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        const auto digest = Hash(files[static_cast<size_t>(index)].data);
        std::copy(digest.begin(), digest.end(), sha256_out);
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t Open(const char* name, bundlefs_file_t& file_out) override {
        const int index = name == nullptr ? -1 : FindFile(name);
        if (index < 0) {
            return BUNDLEFS_ERR_NOT_FOUND;
        }
        file_out = {};
        file_out.opaque[0] = static_cast<uint32_t>(index);
        file_out.opaque[1] = tag_;
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t GetFileInfo(const bundlefs_file_t& file, bundlefs_file_info_t& info_out) override {
        const FakeFile* resolved = Resolve(file);
        if (resolved == nullptr) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        info_out = {};
        std::snprintf(info_out.name, sizeof(info_out.name), "%s", resolved->name.c_str());
        info_out.size = resolved->data.size();
        info_out.content_id = resolved->content_id;
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t Read(const bundlefs_file_t& file, uint32_t offset, void* destination,
                                        uint32_t size) override {
        const FakeFile* resolved = Resolve(file);
        if (resolved == nullptr || destination == nullptr || offset > resolved->data.size() ||
            size > resolved->data.size() - offset) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        std::memcpy(destination, resolved->data.data() + offset, size);
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t Map(const bundlefs_file_t& file, uint32_t offset, uint32_t size,
                                       bundlefs_mapping_t& mapping_out) override {
        if (!mappable_) {
            return BUNDLEFS_ERR_UNAVAILABLE;
        }
        const FakeFile* resolved = Resolve(file);
        if (resolved == nullptr || size == 0U || offset > resolved->data.size() ||
            size > resolved->data.size() - offset) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        mapping_out = {.data = resolved->data.data() + offset,
                       .mapping = resolved->data.data() + offset,
                       .size = size,
                       .mapping_handle = 1U};
        ++open_mappings;
        return BUNDLEFS_OK;
    }

    void Unmap(bundlefs_mapping_t& mapping) override {
        if (mapping.mapping_handle != 0U) {
            --open_mappings;
        }
        mapping = {};
    }

    [[nodiscard]] bundlefs_error_t BeginReplace(const char* name, uint32_t size,
                                                bundlefs_writer_t& writer_out) override {
        if (writer_active || name == nullptr || size == 0U) {
            return BUNDLEFS_ERR_BUSY;
        }
        if (FindFile(name) < 0 && file_count >= files.size()) {
            return BUNDLEFS_ERR_TOO_MANY_FILES;
        }
        writer_active = true;
        staged = {.name = name};
        staged.data.reserve(size);
        writer_out = {};
        writer_out.opaque[0] = 1U;
        writer_out.opaque[1] = tag_;
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t Write(bundlefs_writer_t& writer, const void* data, uint32_t size) override {
        if (!ValidWriter(writer) || data == nullptr) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        if (fail_next_write) {
            fail_next_write = false;
            return BUNDLEFS_ERR_IO;
        }
        const auto* bytes = static_cast<const uint8_t*>(data);
        staged.data.insert(staged.data.end(), bytes, bytes + size);
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t OpenStaged(const bundlefs_writer_t& writer, bundlefs_file_t& file_out) override {
        if (!ValidWriter(writer)) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        file_out = {};
        file_out.opaque[0] = UINT32_MAX;
        file_out.opaque[1] = tag_;
        return BUNDLEFS_OK;
    }

    [[nodiscard]] bundlefs_error_t Commit(bundlefs_writer_t& writer,
                                          const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) override {
        if (!ValidWriter(writer) || expected_sha256 == nullptr) {
            return BUNDLEFS_ERR_INVALID_ARGUMENT;
        }
        const auto digest = Hash(staged.data);
        if (!std::equal(digest.begin(), digest.end(), expected_sha256)) {
            return BUNDLEFS_ERR_HASH_MISMATCH;
        }
        staged.content_id = ++next_content_id_;
        const int existing = FindFile(staged.name.c_str());
        if (existing >= 0) {
            files[static_cast<size_t>(existing)] = std::move(staged);
        } else {
            for (uint32_t index = file_count; index > 0U; --index) {
                files[index] = std::move(files[index - 1U]);
            }
            files[0] = std::move(staged);
            ++file_count;
        }
        staged = {};
        writer_active = false;
        writer = {};
        return BUNDLEFS_OK;
    }

    void Abort(bundlefs_writer_t& writer) override {
        if (writer_active) {
            writer_active = false;
            staged = {};
        }
        writer = {};
    }

    [[nodiscard]] bundlefs_error_t Remove(const char* name) override {
        const int index = name == nullptr ? -1 : FindFile(name);
        if (index < 0) {
            return BUNDLEFS_ERR_NOT_FOUND;
        }
        std::move(files.begin() + index + 1, files.begin() + file_count, files.begin() + index);
        files[--file_count] = {};
        ++removals;
        return BUNDLEFS_OK;
    }

    int open_mappings = 0;

   private:
    const FakeFile* Resolve(const bundlefs_file_t& handle) const {
        if (handle.opaque[1] != tag_) {
            return nullptr;
        }
        if (handle.opaque[0] == UINT32_MAX) {
            return writer_active ? &staged : nullptr;
        }
        const uint32_t index = handle.opaque[0];
        return index < file_count ? &files[index] : nullptr;
    }

    bool ValidWriter(const bundlefs_writer_t& writer) const {
        return writer_active && writer.opaque[0] == 1U && writer.opaque[1] == tag_;
    }

    uint32_t capacity_bytes_;
    bool mappable_;
    uint32_t tag_;
    uint32_t next_content_id_ = 0U;
};

constexpr uint32_t kNorCapacity = 24U * 1024U * 1024U;
constexpr uint32_t kNandCapacity = 120U * 1024U * 1024U;

uint32_t HeaderHash(const micropixel_bundle_header_t& input) {
    micropixel_bundle_header_t header = input;
    header.header_hash = 0U;
    uint32_t value = 2166136261U;
    for (const uint8_t byte : std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&header), sizeof(header))) {
        value ^= byte;
        value *= 16777619U;
    }
    return value;
}

std::vector<uint8_t> MakeBundle(std::string_view app_id, uint8_t fill, uint32_t aot_target_mask = kMatchingTarget) {
    std::vector<uint8_t> bundle(MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT, fill);
    const bool component = app_id.starts_with("fonts.");
    micropixel_bundle_header_t header{};
    std::memcpy(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic));
    header.version = MICROPIXEL_BUNDLE_VERSION;
    header.header_size = sizeof(header);
    header.bundle_size = bundle.size();
    header.toc_offset = sizeof(header);
    header.app_id_length = app_id.size();
    header.section_count = 1U;
    header.framework_abi_version = MICROPIXEL_BUNDLE_FRAMEWORK_ABI_VERSION;
    std::memcpy(header.app_id, app_id.data(), app_id.size());
    micropixel_bundle_section_t section{};
    section.kind = component ? MICROPIXEL_BUNDLE_SECTION_APP_METADATA : MICROPIXEL_BUNDLE_SECTION_AOT;
    section.reserved0 = component ? MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE : aot_target_mask;
    std::memcpy(bundle.data() + header.toc_offset, &section, sizeof(section));
    header.header_hash = HeaderHash(header);
    std::memcpy(bundle.data(), &header, sizeof(header));
    return bundle;
}

micropixel::runtime::AppInstallRequest Request(const std::vector<uint8_t>& bundle, const char* app_id) {
    return micropixel::runtime::AppInstallRequest{
        .data = bundle.data(),
        .size = bundle.size(),
        .expected_app_id = app_id,
        .expected_sha256 = Hash(bundle),
    };
}

void TestEmptyInstallUpdateAndRemove() {
    FakeStore fake(kNorCapacity);
    micropixel::runtime::AppStore store(fake);
    Check(!store.split() && &store.system_store() == &fake && &store.app_store() == &fake,
          "a single medium serves both store roles");
    micropixel::runtime::InstalledAppCatalog catalog{};
    auto catalog_result = store.LoadCatalog(catalog);
    Check(catalog_result.has_value() && catalog.count == 0U, "empty BundleFS must produce an empty App catalog");

    auto first_bundle = MakeBundle("demo", 0x31U);
    auto installed = store.Install(Request(first_bundle, "demo"));
    Check(installed.has_value() && installed->changed && std::strcmp(installed->app.app_id.data(), "demo") == 0,
          "valid Bundle must install through BundleFS");

    catalog_result = store.LoadCatalog(catalog);
    Check(catalog_result.has_value() && catalog.count == 1U &&
              catalog.store_used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE + first_bundle.size() &&
              catalog.apps[0].sha256 == Hash(first_bundle),
          "installed Bundle must be listed with metadata-inclusive Store usage and Catalog SHA-256");
    installed = store.Install(Request(first_bundle, "demo"));
    Check(installed.has_value() && !installed->changed && fake.file_count == 1U,
          "installing the same Bundle digest must converge without rewriting it");

    auto second_bundle = MakeBundle("demo", 0x72U);
    installed = store.Install(Request(second_bundle, "demo"));
    Check(installed.has_value() && installed->changed && fake.file_count == 1U && fake.files[0].data == second_bundle,
          "same AppId must atomically replace the old file");

    auto invalid_request = Request(first_bundle, "demo");
    invalid_request.expected_sha256[0] ^= 0xffU;
    Check(store.Install(invalid_request).error() == micropixel::runtime::AppStoreError::kHashMismatch &&
              fake.files[0].data == second_bundle,
          "package digest failure must be detected before the installed version is removed");

    fake.fail_next_write = true;
    Check(store.Install(Request(first_bundle, "demo")).error() == micropixel::runtime::AppStoreError::kFlashWrite &&
              fake.file_count == 1U && fake.files[0].data == second_bundle && !fake.writer_active,
          "failed replacement must retain the old Bundle without a lingering writer");
    installed = store.Install(Request(second_bundle, "demo"));
    Check(installed.has_value() && !installed->changed && fake.file_count == 1U,
          "old version survives failed replacement");
    Check(store.UninstallApp("demo").has_value(), "installed App must uninstall");
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 0U, "uninstalled App must disappear");
}

void TestReplacementRetainsOldVersionWhenFull() {
    FakeStore fake(kNorCapacity);
    micropixel::runtime::AppStore store(fake);
    // The fake store has 24 MiB. Replacement must have room for both versions.
    constexpr size_t kLargeSize = 20U * 1024U * 1024U;
    auto first = MakeBundle("large", 0x11U);
    first.resize(kLargeSize, 0x11U);
    micropixel_bundle_header_t header{};
    std::memcpy(&header, first.data(), sizeof(header));
    header.bundle_size = first.size();
    header.header_hash = HeaderHash(header);
    std::memcpy(first.data(), &header, sizeof(header));
    Check(store.Install(Request(first, "large")).has_value(), "large first version must install");

    auto second = first;
    std::fill(second.begin() + sizeof(header) + sizeof(micropixel_bundle_section_t), second.end(), 0x22U);
    auto installed = store.Install(Request(second, "large"));
    Check(installed.error() == micropixel::runtime::AppStoreError::kNoSpace && fake.file_count == 1U &&
              fake.files[0].data == first,
          "replacement without spare space must preserve the old version");

    auto oversized = second;
    oversized.resize(30U * 1024U * 1024U, 0x33U);
    std::memcpy(&header, oversized.data(), sizeof(header));
    header.bundle_size = oversized.size();
    header.header_hash = HeaderHash(header);
    std::memcpy(oversized.data(), &header, sizeof(header));
    Check(store.Install(Request(oversized, "large")).error() == micropixel::runtime::AppStoreError::kNoSpace &&
              fake.file_count == 1U && fake.files[0].data == first,
          "oversized replacement must preserve the old version");
}

void TestIdentityAndCapacityErrors() {
    FakeStore fake(kNorCapacity);
    micropixel::runtime::AppStore store(fake);
    auto bundle = MakeBundle("actual", 0x55U);
    Check(store.Install(Request(bundle, "expected")).error() == micropixel::runtime::AppStoreError::kAppIdMismatch,
          "request AppId must match Bundle header");
    Check(store.UninstallApp("missing").error() == micropixel::runtime::AppStoreError::kNotFound,
          "missing App uninstall must be reported");

    auto incompatible = MakeBundle("other", 0x61U, kOtherTarget);
    Check(store.Install(Request(incompatible, "other")).error() ==
                  micropixel::runtime::AppStoreError::kIncompatibleAotTarget &&
              !fake.writer_active && fake.staged.data.empty(),
          "wrong-architecture AOT must be rejected before BundleFS staging begins");
    auto legacy = MakeBundle("legacy", 0x62U, MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE);
    Check(store.Install(Request(legacy, "legacy")).error() ==
                  micropixel::runtime::AppStoreError::kIncompatibleAotTarget &&
              !fake.writer_active && fake.staged.data.empty(),
          "legacy App without AOT target metadata must be rejected before BundleFS staging begins");
    auto ambiguous =
        MakeBundle("ambiguous", 0x63U,
                   MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F | MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3);
    Check(
        store.Install(Request(ambiguous, "ambiguous")).error() == micropixel::runtime::AppStoreError::kInvalidPackage &&
            !fake.writer_active && fake.staged.data.empty(),
        "one AOT payload cannot claim multiple architecture bits");
}

void TestNewestInstallIsListedFirst() {
    FakeStore fake(kNorCapacity);
    micropixel::runtime::AppStore store(fake);
    auto blocks = MakeBundle("blocks", 0x31U);
    auto snake = MakeBundle("snake", 0x42U);
    auto demo = MakeBundle("demo", 0x53U);
    Check(store.Install(Request(blocks, "blocks")).has_value(), "Blocks must install");
    Check(store.Install(Request(snake, "snake")).has_value(), "Snake must install");
    Check(store.Install(Request(demo, "demo")).has_value(), "Demo must install");

    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 3U, "three installed Apps must load");
    Check(std::strcmp(catalog.apps[0].app_id.data(), "demo") == 0 &&
              std::strcmp(catalog.apps[1].app_id.data(), "snake") == 0 &&
              std::strcmp(catalog.apps[2].app_id.data(), "blocks") == 0,
          "App Store catalog must expose newest installs first");

    snake = MakeBundle("snake", 0x64U);
    Check(store.Install(Request(snake, "snake")).has_value(), "Snake update must install");
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 3U &&
              std::strcmp(catalog.apps[0].app_id.data(), "demo") == 0 &&
              std::strcmp(catalog.apps[1].app_id.data(), "snake") == 0 &&
              std::strcmp(catalog.apps[2].app_id.data(), "blocks") == 0,
          "atomic replacement preserves the existing catalog order");
}

void TestComponentTrustVisibilityAndProtection() {
    FakeStore fake(kNorCapacity);
    micropixel::runtime::AppStore store(fake);
    auto component = MakeBundle("fonts.fixture", 0x68U);
    auto request = Request(component, "fonts.fixture");
    Check(store.Install(request).error() == micropixel::runtime::AppStoreError::kUntrustedComponent,
          "Component install must require a verified publisher signature");
    request.trusted_component_signature = true;
    auto installed = store.Install(request);
    Check(installed.has_value() && installed->package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT,
          "trusted Component must install as a non-App package");

    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 0U && catalog.component_count == 1U,
          "Component must count toward Storage without appearing in the App catalog");
    Check(store.UninstallApp("fonts.fixture").error() == micropixel::runtime::AppStoreError::kNotFound,
          "App uninstall path must not remove a Component");
    Check(store.UninstallComponent("fonts.fixture", "fonts.fixture").error() ==
              micropixel::runtime::AppStoreError::kComponentActive,
          "active Component must be protected from uninstall");
    Check(store.UninstallComponent("fonts.fixture").has_value(),
          "inactive Component must uninstall through its explicit path");
}

void TestSplitStoresRouteByPackageType() {
    FakeStore system(kNorCapacity, true, 1U);
    FakeStore downloads(kNandCapacity, false, 2U);
    auto factory = MakeBundle("factory", 0x21U);
    system.Seed("factory", factory);
    micropixel::runtime::AppStore store(system, &downloads);
    Check(!store.split() && store.external_state() == micropixel::runtime::ExternalStorageState::kUnavailable &&
              &store.app_store() == &system,
          "an external store is not trusted before the first catalog load");

    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 1U &&
              std::strcmp(catalog.apps[0].app_id.data(), "factory") == 0 &&
              catalog.apps[0].storage == micropixel::runtime::AppStorage::kSystem &&
              catalog.external_state == micropixel::runtime::ExternalStorageState::kReady &&
              catalog.store_total_bytes == kNorCapacity + kNandCapacity &&
              catalog.store_used_bytes == 2U * MICROPIXEL_BUNDLEFS_METADATA_SIZE + factory.size() &&
              catalog.system_storage.total_bytes == kNorCapacity &&
              catalog.system_storage.used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE + factory.size() &&
              catalog.external_storage.total_bytes == kNandCapacity &&
              catalog.external_storage.used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE,
          "the catalog must merge both stores, sum their usage and report each medium separately");
    Check(store.split() && &store.system_store() == &system && &store.app_store() == &downloads,
          "two media must be exposed as distinct store roles once the external store mounts");

    auto game = MakeBundle("game", 0x31U);
    auto installed = store.Install(Request(game, "game"));
    Check(installed.has_value() && installed->changed && downloads.file_count == 1U && system.file_count == 1U &&
              downloads.files[0].data == game && installed->app.storage == micropixel::runtime::AppStorage::kExternal,
          "downloaded Apps must land on the App store medium and be tagged with it");

    auto component = MakeBundle("fonts.fixture", 0x68U);
    auto component_request = Request(component, "fonts.fixture");
    component_request.trusted_component_signature = true;
    installed = store.Install(component_request);
    Check(installed.has_value() && installed->package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT &&
              system.file_count == 2U && downloads.file_count == 1U && system.files[0].data == component,
          "Components must always install on the system store");

    // Converge against the factory copy without duplicating it on the App store.
    installed = store.Install(Request(factory, "factory"));
    Check(installed.has_value() && !installed->changed && downloads.file_count == 1U,
          "an identical Bundle already on the system store must not be copied to the App store");

    // Updating a factory App moves it to the App store and retires the NOR copy.
    auto factory_update = MakeBundle("factory", 0x22U);
    installed = store.Install(Request(factory_update, "factory"));
    Check(installed.has_value() && installed->changed && downloads.file_count == 2U && system.file_count == 1U &&
              system.FindFile("factory") < 0 && downloads.files[0].data == factory_update && system.removals == 1U,
          "an App update must be committed to the App store before the stale system copy is removed");
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 2U && catalog.component_count == 1U &&
              std::strcmp(catalog.apps[0].app_id.data(), "factory") == 0 &&
              catalog.apps[0].sha256 == Hash(factory_update),
          "the merged catalog must expose the updated App exactly once");

    // A failed replacement on the App store must leave both media untouched.
    downloads.fail_next_write = true;
    Check(store.Install(Request(MakeBundle("game", 0x32U), "game")).error() ==
                  micropixel::runtime::AppStoreError::kFlashWrite &&
              !downloads.writer_active && !system.writer_active && downloads.FindFile("game") >= 0,
          "write failures on the App store must not disturb either medium");

    Check(store.UninstallApp("game").has_value() && downloads.FindFile("game") < 0,
          "uninstall must remove the App from the medium that holds it");
    Check(store.UninstallApp("fonts.fixture").error() == micropixel::runtime::AppStoreError::kNotFound &&
              store.UninstallComponent("fonts.fixture").has_value() && system.file_count == 0U,
          "Component removal must work through the system store");

    // A duplicate AppId visible on both media is reported once, preferring the App store copy.
    system.Seed("factory", factory);
    Check(
        store.LoadCatalog(catalog).has_value() && catalog.count == 1U && catalog.apps[0].sha256 == Hash(factory_update),
        "duplicate AppIds across stores must not produce duplicate catalog entries");

    // A damaged App store medium must not hide the system store.
    downloads.mount_error = BUNDLEFS_ERR_CORRUPT;
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 1U && catalog.apps[0].sha256 == Hash(factory) &&
              catalog.store_total_bytes == kNorCapacity &&
              catalog.external_state == micropixel::runtime::ExternalStorageState::kCorrupt &&
              catalog.external_storage.total_bytes == 0U && !store.split() && &store.app_store() == &system,
          "an unmountable App store must degrade to the system store catalog and report its state");

    // Until the user formats it, downloaded Apps fall back to the system store.
    auto fallback = MakeBundle("fallback", 0x41U);
    installed = store.Install(Request(fallback, "fallback"));
    Check(installed.has_value() && installed->changed && system.FindFile("fallback") >= 0 &&
              installed->app.storage == micropixel::runtime::AppStorage::kSystem && downloads.formats == 0U,
          "an App installed while the external store is not ready lands on the system store");

    downloads.mount_error = BUNDLEFS_ERR_NOT_FORMATTED;
    Check(store.LoadCatalog(catalog).has_value() &&
              catalog.external_state == micropixel::runtime::ExternalStorageState::kNotFormatted,
          "an unformatted external store is reported distinctly from a damaged one");

    // A failed format leaves the state as the medium reports it.
    downloads.format_error = BUNDLEFS_ERR_IO;
    Check(store.FormatExternalStore().error() == micropixel::runtime::AppStoreError::kFlashWrite &&
              downloads.formats == 1U && !store.split() &&
              store.external_state() == micropixel::runtime::ExternalStorageState::kNotFormatted,
          "a format failure must not pretend the external store is ready");
    downloads.format_error = BUNDLEFS_OK;

    // A user-confirmed format empties the external store and brings it online.
    Check(store.FormatExternalStore().has_value() && downloads.formats == 2U && downloads.file_count == 0U &&
              store.split() && store.external_state() == micropixel::runtime::ExternalStorageState::kReady &&
              &store.app_store() == &downloads,
          "formatting the external store must mount it empty and route Apps to it again");
    Check(store.LoadCatalog(catalog).has_value() && catalog.count == 2U &&
              catalog.external_state == micropixel::runtime::ExternalStorageState::kReady &&
              catalog.external_storage.used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE,
          "the catalog after a format lists the untouched system Apps and an empty external store");
    installed = store.Install(Request(fallback, "fallback"));
    Check(installed.has_value() && !installed->changed && downloads.file_count == 0U,
          "an identical App already on the system store is not copied once the external store is ready");

    system.mount_error = BUNDLEFS_ERR_CORRUPT;
    Check(store.LoadCatalog(catalog).error() == micropixel::runtime::AppStoreError::kCatalogCorrupt,
          "a damaged system store is still a hard failure");
    system.mount_error = BUNDLEFS_OK;

    FakeStore single(kNorCapacity, true, 3U);
    micropixel::runtime::AppStore single_store(single);
    Check(single_store.external_state() == micropixel::runtime::ExternalStorageState::kAbsent &&
              single_store.FormatExternalStore().error() == micropixel::runtime::AppStoreError::kUnavailable &&
              single.formats == 0U,
          "a board without external storage cannot format anything");
}

}  // namespace

extern "C" {

// The fake reader only depends on the Bundle source contract, so it exercises
// the real Bundle store and memory source adapters linked into this test.
bool micropixel_read_bundle_metadata(const micropixel_bundle_source_t* source,
                                     micropixel_bundle_metadata_t* metadata_out) {
    uint32_t size = 0U;
    micropixel_bundle_header_t header{};
    if (metadata_out == nullptr || !micropixel_bundle_source_size(source, &size) || size < sizeof(header) ||
        !micropixel_bundle_source_read(source, 0U, &header, sizeof(header))) {
        return false;
    }
    if (std::memcmp(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MICROPIXEL_BUNDLE_VERSION || header.bundle_size != size || header.app_id_length == 0U ||
        header.app_id_length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) {
        return false;
    }
    *metadata_out = {
        .bundle_size = header.bundle_size,
        .metadata_schema_version = MICROPIXEL_BUNDLE_METADATA_SCHEMA_VERSION,
        .package_type = MICROPIXEL_BUNDLE_PACKAGE_APP,
    };
    std::memcpy(metadata_out->app_id, header.app_id, header.app_id_length);
    if (std::strncmp(reinterpret_cast<const char*>(metadata_out->app_id), "fonts.", 6U) == 0) {
        metadata_out->package_type = MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
        metadata_out->component_type = MICROPIXEL_BUNDLE_COMPONENT_FONT;
    }
    constexpr char kDisplayName[] = "Test App";
    std::memcpy(metadata_out->display_name, kDisplayName, sizeof(kDisplayName));
    return true;
}

bool micropixel_read_bundle_metadata_for_locale(const micropixel_bundle_source_t* source, const char*,
                                                micropixel_bundle_metadata_t* metadata_out) {
    return micropixel_read_bundle_metadata(source, metadata_out);
}

bool micropixel_validate_app_package(const micropixel_bundle_source_t* source,
                                     micropixel_bundle_metadata_t* metadata_out) {
    micropixel_bundle_metadata_t metadata{};
    if (!micropixel_read_bundle_metadata(source, &metadata) || metadata.package_type != MICROPIXEL_BUNDLE_PACKAGE_APP) {
        return false;
    }
    if (metadata_out != nullptr) {
        *metadata_out = metadata;
    }
    return true;
}

bool micropixel_validate_component_package(const micropixel_bundle_source_t* source,
                                           micropixel_bundle_metadata_t* metadata_out) {
    return metadata_out != nullptr && micropixel_read_bundle_metadata(source, metadata_out) &&
           metadata_out->package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
}

}  // extern "C"

int main() {
    TestEmptyInstallUpdateAndRemove();
    TestReplacementRetainsOldVersionWhenFull();
    TestIdentityAndCapacityErrors();
    TestNewestInstallIsListedFirst();
    TestComponentTrustVisibilityAndProtection();
    TestSplitStoresRouteByPackageType();
    std::printf("App Store tests passed (%u checks).\n", checks);
    return 0;
}
