#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "psa/crypto.h"
#include "runtime/bundle/app_store.hpp"
#include "runtime/bundle/bundle_format.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/bundlefs/bundlefs.h"
#include "runtime/bundlefs/bundlefs_format.h"

namespace {

struct FakeFile final {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t content_id{};
};

std::array<FakeFile, BUNDLEFS_MAX_FILES> files;
uint32_t file_count = 0U;
FakeFile staged;
bool writer_active = false;
uint32_t checks = 0U;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

int FindFile(const char* name) {
    for (uint32_t index = 0U; index < file_count; ++index) {
        if (files[index].name == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

const FakeFile* ResolveFile(const bundlefs_file_t* handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    if (handle->opaque[0] == UINT32_MAX) {
        return writer_active ? &staged : nullptr;
    }
    const uint32_t index = handle->opaque[0];
    return index < file_count ? &files[index] : nullptr;
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

std::vector<uint8_t> MakeBundle(std::string_view app_id, uint8_t fill) {
    std::vector<uint8_t> bundle(MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT, fill);
    micropixel_bundle_header_t header{};
    std::memcpy(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic));
    header.version = MICROPIXEL_BUNDLE_VERSION;
    header.header_size = sizeof(header);
    header.bundle_size = bundle.size();
    header.app_id_length = app_id.size();
    std::memcpy(header.app_id, app_id.data(), app_id.size());
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

void Reset() {
    files = {};
    file_count = 0U;
    staged = {};
    writer_active = false;
}

void TestEmptyInstallUpdateAndRemove() {
    Reset();
    micropixel::runtime::InstalledAppCatalog catalog{};
    auto catalog_result = micropixel::runtime::LoadAppStoreCatalog(catalog);
    Check(catalog_result.has_value() && catalog.count == 0U, "empty BundleFS must produce an empty App catalog");

    auto first_bundle = MakeBundle("demo", 0x31U);
    auto installed = micropixel::runtime::InstallApp(Request(first_bundle, "demo"));
    Check(installed.has_value() && installed->changed && std::strcmp(installed->app.app_id.data(), "demo") == 0,
          "valid Bundle must install through BundleFS");

    catalog_result = micropixel::runtime::LoadAppStoreCatalog(catalog);
    Check(catalog_result.has_value() && catalog.count == 1U &&
              catalog.store_used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE + first_bundle.size(),
          "installed Bundle must be listed with metadata-inclusive Store usage");
    installed = micropixel::runtime::InstallApp(Request(first_bundle, "demo"));
    Check(installed.has_value() && !installed->changed && file_count == 1U,
          "installing the same Bundle digest must converge without rewriting it");

    auto second_bundle = MakeBundle("demo", 0x72U);
    installed = micropixel::runtime::InstallApp(Request(second_bundle, "demo"));
    Check(installed.has_value() && installed->changed && file_count == 1U && files[0].data == second_bundle,
          "same AppId must atomically replace one BundleFS file");

    auto invalid_request = Request(first_bundle, "demo");
    invalid_request.expected_sha256[0] ^= 0xffU;
    Check(
        micropixel::runtime::InstallApp(invalid_request).error() == micropixel::runtime::AppStoreError::kHashMismatch &&
            files[0].data == second_bundle,
        "hash failure must leave the active file unchanged");

    Check(micropixel::runtime::UninstallApp("demo").has_value(), "installed App must uninstall");
    Check(micropixel::runtime::LoadAppStoreCatalog(catalog).has_value() && catalog.count == 0U,
          "uninstalled App must disappear");
}

void TestIdentityAndCapacityErrors() {
    Reset();
    auto bundle = MakeBundle("actual", 0x55U);
    Check(micropixel::runtime::InstallApp(Request(bundle, "expected")).error() ==
              micropixel::runtime::AppStoreError::kAppIdMismatch,
          "request AppId must match Bundle header");
    Check(micropixel::runtime::UninstallApp("missing").error() == micropixel::runtime::AppStoreError::kNotFound,
          "missing App uninstall must be reported");
}

void TestNewestInstallIsListedFirst() {
    Reset();
    auto blocks = MakeBundle("blocks", 0x31U);
    auto snake = MakeBundle("snake", 0x42U);
    auto demo = MakeBundle("demo", 0x53U);
    Check(micropixel::runtime::InstallApp(Request(blocks, "blocks")).has_value(), "Blocks must install");
    Check(micropixel::runtime::InstallApp(Request(snake, "snake")).has_value(), "Snake must install");
    Check(micropixel::runtime::InstallApp(Request(demo, "demo")).has_value(), "Demo must install");

    micropixel::runtime::InstalledAppCatalog catalog{};
    Check(micropixel::runtime::LoadAppStoreCatalog(catalog).has_value() && catalog.count == 3U,
          "three installed Apps must load");
    Check(std::strcmp(catalog.apps[0].app_id.data(), "demo") == 0 &&
              std::strcmp(catalog.apps[1].app_id.data(), "snake") == 0 &&
              std::strcmp(catalog.apps[2].app_id.data(), "blocks") == 0,
          "App Store catalog must expose newest installs first");

    snake = MakeBundle("snake", 0x64U);
    Check(micropixel::runtime::InstallApp(Request(snake, "snake")).has_value(), "Snake update must install");
    Check(micropixel::runtime::LoadAppStoreCatalog(catalog).has_value() &&
              std::strcmp(catalog.apps[0].app_id.data(), "demo") == 0 &&
              std::strcmp(catalog.apps[1].app_id.data(), "snake") == 0,
          "updating an App must preserve its catalog position");
}

}  // namespace

extern "C" {

bundlefs_error_t bundlefs_mount(void) { return BUNDLEFS_OK; }

bundlefs_error_t bundlefs_get_store_info(bundlefs_store_info_t* info_out) {
    if (info_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    uint32_t bundle_bytes = 0U;
    for (uint32_t index = 0U; index < file_count; ++index) {
        bundle_bytes += files[index].data.size();
    }
    constexpr uint32_t kPartitionSize = 24U * 1024U * 1024U;
    const uint32_t used = MICROPIXEL_BUNDLEFS_METADATA_SIZE + bundle_bytes;
    *info_out = {
        .data_block_size = MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT,
        .total_bytes = kPartitionSize,
        .used_bytes = used,
        .free_bytes = kPartitionSize - used,
        .total_blocks = 383U,
        .used_blocks = static_cast<uint16_t>(bundle_bytes / MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT),
        .file_count = static_cast<uint16_t>(file_count),
    };
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_list(bundlefs_file_info_t* files_out, uint32_t capacity, uint32_t* count_out) {
    if (files_out == nullptr || count_out == nullptr || capacity < file_count) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    *count_out = file_count;
    for (uint32_t index = 0U; index < file_count; ++index) {
        files_out[index] = {};
        std::snprintf(files_out[index].name, sizeof(files_out[index].name), "%s", files[index].name.c_str());
        files_out[index].size = files[index].data.size();
        files_out[index].content_id = files[index].content_id;
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_get_file_sha256(const char* name, uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) {
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

bundlefs_error_t bundlefs_open(const char* name, bundlefs_file_t* file_out) {
    const int index = name == nullptr ? -1 : FindFile(name);
    if (index < 0 || file_out == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    *file_out = {};
    file_out->opaque[0] = static_cast<uint32_t>(index);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_get_file_info(const bundlefs_file_t* file, bundlefs_file_info_t* info_out) {
    const FakeFile* resolved = ResolveFile(file);
    if (resolved == nullptr || info_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    *info_out = {};
    std::snprintf(info_out->name, sizeof(info_out->name), "%s", resolved->name.c_str());
    info_out->size = resolved->data.size();
    info_out->content_id = resolved->content_id;
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_begin_replace(const char* name, uint32_t size, bundlefs_writer_t* writer_out) {
    if (writer_active || name == nullptr || writer_out == nullptr || size == 0U) {
        return BUNDLEFS_ERR_BUSY;
    }
    if (FindFile(name) < 0 && file_count >= files.size()) {
        return BUNDLEFS_ERR_TOO_MANY_FILES;
    }
    writer_active = true;
    staged = {.name = name};
    staged.data.reserve(size);
    *writer_out = {};
    writer_out->opaque[0] = 1U;
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_write(bundlefs_writer_t* writer, const void* data, uint32_t size) {
    if (!writer_active || writer == nullptr || writer->opaque[0] != 1U || data == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    staged.data.insert(staged.data.end(), bytes, bytes + size);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_open_staged(const bundlefs_writer_t* writer, bundlefs_file_t* file_out) {
    if (!writer_active || writer == nullptr || writer->opaque[0] != 1U || file_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    *file_out = {};
    file_out->opaque[0] = UINT32_MAX;
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_commit(bundlefs_writer_t* writer, const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) {
    if (!writer_active || writer == nullptr || writer->opaque[0] != 1U || expected_sha256 == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const auto digest = Hash(staged.data);
    if (!std::equal(digest.begin(), digest.end(), expected_sha256)) {
        return BUNDLEFS_ERR_HASH_MISMATCH;
    }
    staged.content_id = (static_cast<uint32_t>(digest[0]) << 24U) | (static_cast<uint32_t>(digest[1]) << 16U) |
                        (static_cast<uint32_t>(digest[2]) << 8U) | digest[3];
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
    *writer = {};
    return BUNDLEFS_OK;
}

void bundlefs_abort(bundlefs_writer_t* writer) {
    if (writer_active) {
        writer_active = false;
        staged = {};
    }
    if (writer != nullptr) {
        *writer = {};
    }
}

bundlefs_error_t bundlefs_remove(const char* name) {
    const int index = name == nullptr ? -1 : FindFile(name);
    if (index < 0) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    std::move(files.begin() + index + 1, files.begin() + file_count, files.begin() + index);
    files[--file_count] = {};
    return BUNDLEFS_OK;
}

bool micropixel_read_bundle_metadata(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out) {
    const FakeFile* resolved = ResolveFile(file);
    if (resolved == nullptr || metadata_out == nullptr || resolved->data.size() < sizeof(micropixel_bundle_header_t)) {
        return false;
    }
    micropixel_bundle_header_t header{};
    std::memcpy(&header, resolved->data.data(), sizeof(header));
    if (std::memcmp(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MICROPIXEL_BUNDLE_VERSION || header.bundle_size != resolved->data.size() ||
        header.app_id_length == 0U || header.app_id_length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) {
        return false;
    }
    *metadata_out = {.bundle_size = header.bundle_size};
    std::memcpy(metadata_out->app_id, header.app_id, header.app_id_length);
    constexpr char kDisplayName[] = "Test App";
    std::memcpy(metadata_out->display_name, kDisplayName, sizeof(kDisplayName));
    return true;
}

bool micropixel_open_aot_package(const bundlefs_file_t* file, micropixel_aot_package_t* package_out) {
    micropixel_bundle_metadata_t metadata{};
    if (package_out == nullptr || !micropixel_read_bundle_metadata(file, &metadata)) {
        return false;
    }
    *package_out = {};
    return true;
}

void micropixel_close_aot_package(micropixel_aot_package_t* package) {
    if (package != nullptr) {
        *package = {};
    }
}

}  // extern "C"

int main() {
    TestEmptyInstallUpdateAndRemove();
    TestIdentityAndCapacityErrors();
    TestNewestInstallIsListedFirst();
    std::printf("App Store tests passed (%u checks).\n", checks);
    return 0;
}
