#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/bundle/bundle_reader.h"

static uint8_t* test_bundle;
static uint32_t test_bundle_size;
static uint32_t active_mappings;
static uint32_t last_mapping_offset;
static uint32_t last_mapping_size;
static uint32_t next_mapping_handle = 1U;

static bool check(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

static bool load_bundle(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    const long length = ftell(file);
    if (length <= 0 || (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    test_bundle = malloc((size_t)length);
    if (test_bundle == NULL || fread(test_bundle, 1U, (size_t)length, file) != (size_t)length) {
        free(test_bundle);
        test_bundle = NULL;
        fclose(file);
        return false;
    }
    fclose(file);
    test_bundle_size = (uint32_t)length;
    return true;
}

bundlefs_error_t bundlefs_get_file_info(const bundlefs_file_t* file, bundlefs_file_info_t* info_out) {
    if (file == NULL || info_out == NULL || file->opaque[0] != 1U) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    memset(info_out, 0, sizeof(*info_out));
    info_out->size = test_bundle_size;
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_read(const bundlefs_file_t* file, uint32_t offset, void* destination, uint32_t size) {
    if (file == NULL || file->opaque[0] != 1U || destination == NULL || offset > test_bundle_size ||
        size > test_bundle_size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    memcpy(destination, test_bundle + offset, size);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_mmap(const bundlefs_file_t* file, uint32_t offset, uint32_t size,
                               bundlefs_mapping_t* mapping_out) {
    if (file == NULL || file->opaque[0] != 1U || mapping_out == NULL || size == 0U || offset > test_bundle_size ||
        size > test_bundle_size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    *mapping_out = (bundlefs_mapping_t){
        .data = test_bundle + offset,
        .mapping = test_bundle + offset,
        .size = size,
        .mapping_handle = next_mapping_handle++,
    };
    ++active_mappings;
    last_mapping_offset = offset;
    last_mapping_size = size;
    return BUNDLEFS_OK;
}

void bundlefs_munmap(bundlefs_mapping_t* mapping) {
    if (mapping != NULL && mapping->mapping != NULL && active_mappings > 0U) {
        --active_mappings;
    }
    if (mapping != NULL) {
        memset(mapping, 0, sizeof(*mapping));
    }
}

static bool validate_bundle(void) {
    const bundlefs_file_t file = {.opaque = {1U}};
    micropixel_bundle_metadata_t metadata;
    if (!check(micropixel_read_bundle_metadata(&file, &metadata), "Bundle metadata must validate") ||
        !check(metadata.bundle_size == test_bundle_size, "metadata must expose the Bundle logical size") ||
        !check(metadata.app_id[0] != 0U && metadata.display_name[0] != 0U, "metadata must contain App identity")) {
        return false;
    }

    micropixel_bundle_asset_mapping_t cover;
    if (!check(micropixel_open_launch_asset(&file, &cover), "launch cover must map") ||
        !check(active_mappings == 1U, "cover path must own exactly one mapping") ||
        !check(last_mapping_offset > 0U, "cover mapping must begin at the asset, not Bundle start") ||
        !check(last_mapping_size == cover.asset.size && last_mapping_size < metadata.bundle_size,
               "Hall must map only the launch asset") ||
        !check(cover.asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG, "production Hall cover must stay compressed") ||
        !check(cover.asset.content_hash != 0U, "Hall cover identity must include its content hash")) {
        return false;
    }
    const uint32_t cover_offset = (uint32_t)(cover.asset.data - test_bundle);
    micropixel_close_asset_mapping(&cover);
    if (!check(active_mappings == 0U, "closing a cover must release its mapping")) {
        return false;
    }

    micropixel_aot_package_t package;
    if (!check(micropixel_open_aot_package(&file, &package), "Bundle must open as an AOT package") ||
        !check(active_mappings == 1U, "running package must own one Bundle mapping") ||
        !check(last_mapping_offset == 0U && last_mapping_size == metadata.bundle_size,
               "running package must map its logical Bundle") ||
        !check(package.payload != NULL && package.payload_size > 0U, "relocatable AOT must be copied for WAMR")) {
        return false;
    }
    micropixel_bundle_asset_view_t launch;
    const bool found_launch = micropixel_bundle_find_asset(&package, package.launch_asset_id, &launch);
    micropixel_close_aot_package(&package);
    if (!check(found_launch, "running package must resolve its launch asset") ||
        !check(active_mappings == 0U, "closing a package must release its Bundle mapping")) {
        return false;
    }

    const uint8_t original = test_bundle[cover_offset];
    test_bundle[cover_offset] ^= 0xffU;
    const bool corrupt_opened = micropixel_open_launch_asset(&file, &cover);
    test_bundle[cover_offset] = original;
    if (corrupt_opened) {
        micropixel_close_asset_mapping(&cover);
    }
    return check(!corrupt_opened, "Bundle reader must reject a corrupt cover") &&
           check(active_mappings == 0U, "failed cover validation must not leak a mapping");
}

int main(int argc, char** argv) {
    if (argc != 2 || !load_bundle(argv[1])) {
        fprintf(stderr, "Usage: bundle_reader_test APP_BUNDLE\n");
        return 2;
    }
    const bool passed = validate_bundle();
    free(test_bundle);
    if (!passed) {
        return 1;
    }
    puts("Bundle reader host integration passed (opaque BundleFS file, asset/full mapping and corruption checks). ");
    return 0;
}
