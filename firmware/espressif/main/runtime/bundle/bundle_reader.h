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
    const uint8_t* data;
    uint32_t size;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} micropixel_bundle_asset_view_t;

bool micropixel_open_aot_package(micropixel_aot_package_t* package_out);
void micropixel_close_aot_package(micropixel_aot_package_t* package);
bool micropixel_bundle_find_asset(const micropixel_aot_package_t* package, uint32_t asset_id,
                                  micropixel_bundle_asset_view_t* view_out);

#ifdef __cplusplus
}
#endif

#endif
