#ifndef MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_READER_H
#define MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_READER_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/bundle/bundle_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* payload;
    uint32_t payload_size;
    const uint8_t* bundle_mapping;
    uint32_t bundle_size;
    const micropixel_bundle_section_t* sections;
    uint32_t section_count;
    uint32_t launch_asset_id;
    uint8_t app_id[MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U];
    uint32_t mapping_handle;
} micropixel_aot_package_t;

typedef struct {
    uint32_t store_offset;
    uint32_t bundle_size;
    uint8_t app_id[MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U];
    uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U];
} micropixel_bundle_metadata_t;

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t content_hash;
} micropixel_bundle_asset_view_t;

typedef struct {
    micropixel_bundle_asset_view_t asset;
    const uint8_t* mapping;
    uint32_t mapping_handle;
} micropixel_bundle_asset_mapping_t;

bool micropixel_scan_app_store(micropixel_bundle_metadata_t* apps_out, uint32_t capacity, uint32_t* count_out);
bool micropixel_open_launch_asset(uint32_t store_offset, micropixel_bundle_asset_mapping_t* mapping_out);
void micropixel_close_asset_mapping(micropixel_bundle_asset_mapping_t* mapping);
bool micropixel_open_aot_package(uint32_t store_offset, micropixel_aot_package_t* package_out);
void micropixel_close_aot_package(micropixel_aot_package_t* package);
bool micropixel_bundle_find_asset(const micropixel_aot_package_t* package, uint32_t asset_id,
                                  micropixel_bundle_asset_view_t* view_out);

#ifdef __cplusplus
}
#endif

#endif
