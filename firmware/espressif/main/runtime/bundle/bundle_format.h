#ifndef MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_FORMAT_H
#define MICROPIXEL_RUNTIME_BUNDLE_BUNDLE_FORMAT_H

#include <stdint.h>

#define MICROPIXEL_BUNDLE_VERSION 1U
#define MICROPIXEL_BUNDLE_FRAMEWORK_ABI_VERSION 1U
#define MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT (64U * 1024U)
#define MICROPIXEL_BUNDLE_MAX_SECTIONS 128U
#define MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH 64U
#define MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH 64U
#define MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH 31U
#define MICROPIXEL_BUNDLE_METADATA_MAX_LENGTH 4096U
#define MICROPIXEL_BUNDLE_METADATA_MAX_LOCALES 16U
#define MICROPIXEL_BUNDLE_PACKAGE_VERSION_MAX_LENGTH 31U
#define MICROPIXEL_BUNDLE_FONT_ROLE_COUNT 4U
#define MICROPIXEL_BUNDLE_METADATA_SCHEMA_VERSION 1U

static const uint8_t MICROPIXEL_BUNDLE_MAGIC[8] = {'M', 'P', 'X', 'B', 'N', 'D', 'L', '\0'};

typedef enum micropixel_bundle_section_kind {
    MICROPIXEL_BUNDLE_SECTION_AOT = 1,
    MICROPIXEL_BUNDLE_SECTION_ASSET = 2,
    MICROPIXEL_BUNDLE_SECTION_APP_METADATA = 3,
    MICROPIXEL_BUNDLE_SECTION_FONT = 4,
} micropixel_bundle_section_kind_t;

typedef enum micropixel_bundle_section_format {
    MICROPIXEL_BUNDLE_FORMAT_AOT_RELOCATABLE = 1,
    MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888 = 2,
    MICROPIXEL_BUNDLE_FORMAT_JPEG = 3,
    MICROPIXEL_BUNDLE_FORMAT_PNG = 4,
    MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888 = 5,
    MICROPIXEL_BUNDLE_FORMAT_UTF8 = 6,
    MICROPIXEL_BUNDLE_FORMAT_PACKAGE_METADATA_JSON = 7,
    MICROPIXEL_BUNDLE_FORMAT_LVGL_CBIN_V1 = 8,
    MICROPIXEL_BUNDLE_FORMAT_OGG_OPUS = 9,
    MICROPIXEL_BUNDLE_FORMAT_RAW_RGB565 = 10,
} micropixel_bundle_section_format_t;

typedef enum micropixel_bundle_package_type {
    MICROPIXEL_BUNDLE_PACKAGE_APP = 1,
    MICROPIXEL_BUNDLE_PACKAGE_COMPONENT = 2,
} micropixel_bundle_package_type_t;

typedef enum micropixel_bundle_aot_target_mask {
    /* Stored as a one-bit mask in the reserved0 field of each AOT section. */
    /* No AOT payload, or a legacy App Bundle created before target metadata. */
    MICROPIXEL_BUNDLE_AOT_TARGET_MASK_NONE = 0,
    MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F = 1U << 0U,
    MICROPIXEL_BUNDLE_AOT_TARGET_MASK_XTENSA_ESP32S3 = 1U << 1U,
} micropixel_bundle_aot_target_mask_t;

typedef enum micropixel_bundle_aot_flag {
    /* flags=0 identifies a legacy Bundle without an explicit threading declaration. */
    MICROPIXEL_BUNDLE_AOT_FLAG_THREADING_DECLARED = 1U << 0U,
    MICROPIXEL_BUNDLE_AOT_FLAG_SHARED_MEMORY = 1U << 1U,
    MICROPIXEL_BUNDLE_AOT_FLAG_MASK =
        MICROPIXEL_BUNDLE_AOT_FLAG_THREADING_DECLARED | MICROPIXEL_BUNDLE_AOT_FLAG_SHARED_MEMORY,
} micropixel_bundle_aot_flag_t;

typedef enum micropixel_bundle_display_profile {
    MICROPIXEL_BUNDLE_DISPLAY_SQUARE = 1,
    MICROPIXEL_BUNDLE_DISPLAY_LANDSCAPE = 2,
    MICROPIXEL_BUNDLE_DISPLAY_PORTRAIT = 3,
} micropixel_bundle_display_profile_t;

typedef enum micropixel_bundle_component_type {
    MICROPIXEL_BUNDLE_COMPONENT_NONE = 0,
    MICROPIXEL_BUNDLE_COMPONENT_FONT = 1,
} micropixel_bundle_component_type_t;

typedef enum micropixel_bundle_font_role {
    MICROPIXEL_BUNDLE_FONT_ROLE_SMALL = 0,
    MICROPIXEL_BUNDLE_FONT_ROLE_MEDIUM = 1,
    MICROPIXEL_BUNDLE_FONT_ROLE_LARGE = 2,
    MICROPIXEL_BUNDLE_FONT_ROLE_TITLE = 3,
} micropixel_bundle_font_role_t;

typedef struct __attribute__((packed)) micropixel_bundle_header {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t bundle_size;
    uint32_t toc_offset;
    uint8_t app_id[MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH];
    uint32_t app_id_length;
    uint32_t section_count;
    uint32_t framework_abi_version;
    uint32_t launch_asset_id;
    uint32_t header_hash;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t reserved4;
} micropixel_bundle_header_t;

typedef struct __attribute__((packed)) micropixel_bundle_section {
    uint32_t kind;
    uint32_t id;
    uint32_t offset;
    uint32_t size;
    uint32_t hash;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} micropixel_bundle_section_t;

#if defined(__cplusplus)
static_assert(sizeof(micropixel_bundle_header_t) == 128U, "Bundle header ABI size changed");
static_assert(sizeof(micropixel_bundle_section_t) == 48U, "Bundle section ABI size changed");
#else
_Static_assert(sizeof(micropixel_bundle_header_t) == 128U, "Bundle header ABI size changed");
_Static_assert(sizeof(micropixel_bundle_section_t) == 48U, "Bundle section ABI size changed");
#endif

#endif
