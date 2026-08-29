#ifndef MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_READER_H
#define MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_READER_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/bundle/bundle_format.h"
#include "runtime/bundlefs/bundlefs.h"

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
    uint32_t bundle_size;
    uint32_t metadata_schema_version;
    uint32_t package_type;
    uint32_t display_profile;
    uint32_t component_type;
    uint32_t language_count;
    uint32_t font_asset_ids[MICROPIXEL_BUNDLE_FONT_ROLE_COUNT];
    uint8_t app_id[MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U];
    uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U];
    uint8_t package_version[MICROPIXEL_BUNDLE_PACKAGE_VERSION_MAX_LENGTH + 1U];
    uint8_t languages[MICROPIXEL_BUNDLE_METADATA_MAX_LOCALES][MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH + 1U];
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
    const uint8_t* data;
    uint32_t size;
    uint32_t content_hash;
} micropixel_bundle_font_view_t;

typedef struct {
    micropixel_bundle_asset_view_t asset;
    const uint8_t* mapping;
    uint32_t mapping_handle;
} micropixel_bundle_asset_mapping_t;

bool micropixel_read_bundle_metadata(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out);
bool micropixel_read_bundle_metadata_for_locale(const bundlefs_file_t* file, const char* effective_locale,
                                                micropixel_bundle_metadata_t* metadata_out);
bool micropixel_validate_component_package(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out);
bool micropixel_open_launch_asset(const bundlefs_file_t* file, micropixel_bundle_asset_mapping_t* mapping_out);
void micropixel_close_asset_mapping(micropixel_bundle_asset_mapping_t* mapping);
bool micropixel_open_aot_package(const bundlefs_file_t* file, micropixel_aot_package_t* package_out);
void micropixel_close_aot_package(micropixel_aot_package_t* package);
bool micropixel_bundle_find_asset(const micropixel_aot_package_t* package, uint32_t asset_id,
                                  micropixel_bundle_asset_view_t* view_out);
bool micropixel_bundle_find_font(const micropixel_aot_package_t* package, uint32_t resource_id,
                                 micropixel_bundle_font_view_t* view_out);

#ifdef __cplusplus
}
#endif

#endif
