#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "runtime/bundle/bundle_reader.h"

static esp_partition_t test_partition;
static uint8_t* test_flash;
static uint32_t active_mappings;
static uint32_t last_mapping_offset;
static uint32_t last_mapping_size;
static uint32_t next_mapping_handle = 1U;

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char* label) {
    return type == ESP_PARTITION_TYPE_DATA && subtype == 0x40 && label != NULL && strcmp(label, "app_store") == 0
               ? &test_partition
               : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t source_offset, void* destination, size_t size) {
    if (partition != &test_partition || destination == NULL || source_offset > test_partition.size ||
        size > test_partition.size - source_offset) {
        return -1;
    }
    memcpy(destination, test_flash + source_offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_mmap(const esp_partition_t* partition, size_t offset, size_t size, int memory,
                             const void** mapped_pointer, esp_partition_mmap_handle_t* handle) {
    (void)memory;
    if (partition != &test_partition || mapped_pointer == NULL || handle == NULL || offset > test_partition.size ||
        size > test_partition.size - offset || offset > UINT32_MAX || size > UINT32_MAX) {
        return -1;
    }
    *mapped_pointer = test_flash + offset;
    *handle = next_mapping_handle++;
    ++active_mappings;
    last_mapping_offset = (uint32_t)offset;
    last_mapping_size = (uint32_t)size;
    return ESP_OK;
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle) {
    if (handle != 0U && active_mappings > 0U) {
        --active_mappings;
    }
}

static bool check(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

static bool load_flash(const char* path) {
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
    test_flash = malloc((size_t)length);
    if (test_flash == NULL || fread(test_flash, 1U, (size_t)length, file) != (size_t)length) {
        free(test_flash);
        test_flash = NULL;
        fclose(file);
        return false;
    }
    fclose(file);
    test_partition.address = 0x800000U;
    test_partition.size = (uint32_t)length;
    return true;
}

static bool validate_catalog(micropixel_bundle_metadata_t* apps) {
    uint32_t count = 0U;
    if (!check(micropixel_scan_app_store(apps, 3U, &count), "Firmware scanner must accept the production image") ||
        !check(count == 3U, "erased terminator must stop the catalog after three Apps") ||
        !check(strcmp((const char*)apps[0].app_id, "micropixel.blocks") == 0, "slot 0 must be Blocks") ||
        !check(strcmp((const char*)apps[1].app_id, "micropixel.snake") == 0, "slot 1 must be Snake") ||
        !check(strcmp((const char*)apps[2].app_id, "micropixel.demo") == 0, "slot 2 must be Demo") ||
        !check(strcmp((const char*)apps[0].display_name, "Juicy Blocks") == 0,
               "slot 0 must expose its manifest title") ||
        !check(strcmp((const char*)apps[1].display_name, "Juicy Snake") == 0,
               "slot 1 must expose its manifest title") ||
        !check(strcmp((const char*)apps[2].display_name, "SDK Demo") == 0, "slot 2 must expose its manifest title") ||
        !check(apps[0].store_offset == 0U, "first Bundle must begin at App Store offset zero") ||
        !check(apps[1].store_offset == apps[0].bundle_size, "second Bundle must follow the first extent") ||
        !check(apps[2].store_offset == apps[1].store_offset + apps[1].bundle_size,
               "third Bundle must follow the second extent")) {
        return false;
    }
    return true;
}

static bool validate_app(const micropixel_bundle_metadata_t* app) {
    micropixel_bundle_asset_mapping_t cover;
    if (!check(micropixel_open_launch_asset(app->store_offset, &cover), "launch cover must map") ||
        !check(active_mappings == 1U, "cover path must own exactly one mapping") ||
        !check(last_mapping_offset > app->store_offset, "cover mapping must begin at its asset, not Bundle start") ||
        !check(last_mapping_size == cover.asset.size && last_mapping_size < app->bundle_size,
               "Hall must map only the launch asset") ||
        !check(cover.asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG, "production Hall cover must stay compressed") ||
        !check(cover.asset.width > 0U && cover.asset.height > 0U && cover.asset.stride == 0U && cover.asset.size > 0U,
               "compressed Hall cover metadata must be self-consistent") ||
        !check(cover.asset.content_hash != 0U, "Hall cover cache identity must include its content hash")) {
        return false;
    }
    const uint32_t cover_flash_offset = (uint32_t)(cover.asset.data - test_flash);
    micropixel_close_asset_mapping(&cover);
    if (!check(active_mappings == 0U, "closing a cover must release its Flash mapping")) {
        return false;
    }

    micropixel_aot_package_t package;
    if (!check(micropixel_open_aot_package(app->store_offset, &package), "selected Bundle must open") ||
        !check(active_mappings == 1U, "running package must own exactly one complete Bundle mapping") ||
        !check(last_mapping_offset == app->store_offset && last_mapping_size == app->bundle_size,
               "running package must map its selected Bundle extent") ||
        !check(package.payload != NULL && package.payload_size > 0U, "relocatable AOT must be copied for WAMR")) {
        return false;
    }
    micropixel_bundle_asset_view_t launch;
    const bool found_launch = micropixel_bundle_find_asset(&package, package.launch_asset_id, &launch);
    micropixel_close_aot_package(&package);
    if (!check(found_launch, "running package must resolve its launch asset") ||
        !check(active_mappings == 0U, "closing a package must release its complete Bundle mapping")) {
        return false;
    }

    const uint8_t original = test_flash[cover_flash_offset];
    test_flash[cover_flash_offset] ^= 0xffU;
    const bool corrupt_opened = micropixel_open_launch_asset(app->store_offset, &cover);
    test_flash[cover_flash_offset] = original;
    if (corrupt_opened) {
        micropixel_close_asset_mapping(&cover);
    }
    return check(!corrupt_opened, "Firmware must reject a corrupt Flash cover") &&
           check(active_mappings == 0U, "failed cover validation must not leak a mapping");
}

int main(int argc, char** argv) {
    if (argc != 2 || !load_flash(argv[1])) {
        fprintf(stderr, "Usage: bundle_reader_test APP_STORE_IMAGE\n");
        return 2;
    }
    micropixel_bundle_metadata_t apps[3] = {0};
    const bool passed =
        validate_catalog(apps) && validate_app(&apps[0]) && validate_app(&apps[1]) && validate_app(&apps[2]);
    free(test_flash);
    if (!passed) {
        return 1;
    }
    puts("Bundle reader host integration passed (3 production Apps, cover/full mapping and corruption checks).");
    return 0;
}
