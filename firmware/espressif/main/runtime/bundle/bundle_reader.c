#include "runtime/bundle/bundle_reader.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include "wasm_export.h"

static const char* TAG = "micropixel_package";

static uint32_t fnv1a32(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261U;
    for (size_t index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= 16777619U;
    }
    return hash;
}

static bool valid_app_id(const micropixel_bundle_header_t* header) {
    if (header->app_id_length == 0U || header->app_id_length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) {
        return false;
    }
    for (uint32_t index = 0U; index < header->app_id_length; ++index) {
        const uint8_t byte = header->app_id[index];
        const bool valid = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
        if (!valid) {
            return false;
        }
    }
    for (uint32_t index = header->app_id_length; index < MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH; ++index) {
        if (header->app_id[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool valid_display_name(const uint8_t* bytes, uint32_t length) {
    if (bytes == NULL || length == 0U || length > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH || bytes[0] == ' ' ||
        bytes[length - 1U] == ' ') {
        return false;
    }
    uint32_t index = 0U;
    while (index < length) {
        const uint8_t first = bytes[index];
        uint32_t sequence_length = 0U;
        if (first <= 0x7fU) {
            if (first < 0x20U || first == 0x7fU) {
                return false;
            }
            sequence_length = 1U;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            sequence_length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            sequence_length = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            sequence_length = 4U;
        } else {
            return false;
        }
        if (sequence_length > length - index) {
            return false;
        }
        for (uint32_t continuation = 1U; continuation < sequence_length; ++continuation) {
            if ((bytes[index + continuation] & 0xc0U) != 0x80U) {
                return false;
            }
        }
        if ((first == 0xe0U && bytes[index + 1U] < 0xa0U) || (first == 0xedU && bytes[index + 1U] >= 0xa0U) ||
            (first == 0xf0U && bytes[index + 1U] < 0x90U) || (first == 0xf4U && bytes[index + 1U] >= 0x90U)) {
            return false;
        }
        index += sequence_length;
    }
    return true;
}

static bool valid_launch_asset_section(const micropixel_bundle_section_t* section) {
    if (section == NULL || section->size == 0U || section->width == 0U || section->height == 0U) {
        return false;
    }
    if (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888) {
        return section->stride == section->width * 3U && (uint64_t)section->stride * section->height == section->size;
    }
    return section->format == MICROPIXEL_BUNDLE_FORMAT_PNG && section->stride == 0U;
}

static bool read_logical(const bundlefs_file_t* file, uint32_t logical_offset, void* destination, uint32_t size) {
    return bundlefs_read(file, logical_offset, destination, size) == BUNDLEFS_OK;
}

static bool read_display_name(const bundlefs_file_t* file, const micropixel_bundle_header_t* header,
                              uint8_t* display_name_out) {
    const uint32_t toc_end = header->toc_offset + header->section_count * sizeof(micropixel_bundle_section_t);
    bool metadata_found = false;
    uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U] = {0};
    for (uint32_t index = 0U; index < header->section_count; ++index) {
        micropixel_bundle_section_t section;
        const uint32_t section_offset = header->toc_offset + index * sizeof(section);
        if (!read_logical(file, section_offset, &section, sizeof(section))) {
            return false;
        }
        if (section.kind != MICROPIXEL_BUNDLE_SECTION_APP_METADATA) {
            continue;
        }
        if (metadata_found || section.id != 0U || section.size == 0U ||
            section.size > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH || section.offset < toc_end ||
            (section.offset & 63U) != 0U || section.offset > header->bundle_size ||
            section.size > header->bundle_size - section.offset || section.format != MICROPIXEL_BUNDLE_FORMAT_UTF8 ||
            section.width != 0U || section.height != 0U || section.stride != 0U || section.flags != 0U ||
            section.reserved0 != 0U || section.reserved1 != 0U ||
            !read_logical(file, section.offset, display_name, section.size) ||
            fnv1a32(display_name, section.size) != section.hash || !valid_display_name(display_name, section.size)) {
            return false;
        }
        metadata_found = true;
    }
    if (!metadata_found) {
        return false;
    }
    if (display_name_out != NULL) {
        memcpy(display_name_out, display_name, sizeof(display_name));
    }
    return true;
}

static bool read_valid_header(const bundlefs_file_t* file, micropixel_bundle_header_t* header_out,
                              uint8_t* display_name_out) {
    if (file == NULL || header_out == NULL) {
        return false;
    }
    bundlefs_file_info_t file_info;
    if (bundlefs_get_file_info(file, &file_info) != BUNDLEFS_OK) {
        return false;
    }
    micropixel_bundle_header_t header;
    if (!read_logical(file, 0U, &header, sizeof(header))) {
        return false;
    }
    if (memcmp(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MICROPIXEL_BUNDLE_VERSION || header.header_size != sizeof(header) ||
        header.framework_abi_version != MICROPIXEL_BUNDLE_FRAMEWORK_ABI_VERSION) {
        return false;
    }
    if (header.bundle_size != file_info.size || header.section_count == 0U ||
        header.section_count > MICROPIXEL_BUNDLE_MAX_SECTIONS || header.toc_offset != sizeof(header) ||
        header.section_count > (header.bundle_size - header.toc_offset) / sizeof(micropixel_bundle_section_t) ||
        !valid_app_id(&header) || header.reserved0 != 0U || header.reserved1 != 0U || header.reserved2 != 0U ||
        header.reserved3 != 0U || header.reserved4 != 0U) {
        return false;
    }
    micropixel_bundle_header_t hash_header = header;
    const uint32_t expected_header_hash = hash_header.header_hash;
    hash_header.header_hash = 0U;
    if (fnv1a32((const uint8_t*)&hash_header, sizeof(hash_header)) != expected_header_hash ||
        !read_display_name(file, &header, display_name_out)) {
        return false;
    }
    *header_out = header;
    return true;
}

bool micropixel_read_bundle_metadata(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out) {
    if (metadata_out == NULL) {
        return false;
    }
    memset(metadata_out, 0, sizeof(*metadata_out));
    micropixel_bundle_header_t header;
    uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U] = {0};
    if (!read_valid_header(file, &header, display_name)) {
        return false;
    }
    metadata_out->bundle_size = header.bundle_size;
    memcpy(metadata_out->app_id, header.app_id, header.app_id_length);
    memcpy(metadata_out->display_name, display_name, sizeof(metadata_out->display_name));
    return true;
}

bool micropixel_open_launch_asset(const bundlefs_file_t* file, micropixel_bundle_asset_mapping_t* mapping_out) {
    if (mapping_out == NULL) {
        return false;
    }
    memset(mapping_out, 0, sizeof(*mapping_out));
    micropixel_bundle_header_t header;
    if (!read_valid_header(file, &header, NULL) || header.launch_asset_id == 0U) {
        return false;
    }

    const uint32_t toc_end = header.toc_offset + header.section_count * sizeof(micropixel_bundle_section_t);
    micropixel_bundle_section_t launch_section;
    memset(&launch_section, 0, sizeof(launch_section));
    bool launch_found = false;
    for (uint32_t index = 0U; index < header.section_count; ++index) {
        micropixel_bundle_section_t section;
        const uint32_t section_offset = header.toc_offset + index * sizeof(section);
        if (!read_logical(file, section_offset, &section, sizeof(section))) {
            return false;
        }
        if (section.kind != MICROPIXEL_BUNDLE_SECTION_ASSET || section.id != header.launch_asset_id) {
            continue;
        }
        if (launch_found || section.size == 0U || section.offset < toc_end || (section.offset & 63U) != 0U ||
            section.offset > header.bundle_size || section.size > header.bundle_size - section.offset ||
            !valid_launch_asset_section(&section) || section.flags != 0U || section.reserved0 != 0U ||
            section.reserved1 != 0U) {
            return false;
        }
        launch_section = section;
        launch_found = true;
    }
    if (!launch_found) {
        return false;
    }

    bundlefs_mapping_t bundlefs_mapping;
    if (bundlefs_mmap(file, launch_section.offset, launch_section.size, &bundlefs_mapping) != BUNDLEFS_OK) {
        return false;
    }
    const uint8_t* asset_data = bundlefs_mapping.data;
    if (fnv1a32(asset_data, launch_section.size) != launch_section.hash) {
        bundlefs_munmap(&bundlefs_mapping);
        return false;
    }

    mapping_out->asset.data = asset_data;
    mapping_out->asset.size = launch_section.size;
    mapping_out->asset.format = launch_section.format;
    mapping_out->asset.width = launch_section.width;
    mapping_out->asset.height = launch_section.height;
    mapping_out->asset.stride = launch_section.stride;
    mapping_out->asset.content_hash = launch_section.hash;
    mapping_out->mapping = bundlefs_mapping.mapping;
    mapping_out->mapping_handle = bundlefs_mapping.mapping_handle;
    return true;
}

void micropixel_close_asset_mapping(micropixel_bundle_asset_mapping_t* mapping) {
    if (mapping == NULL) {
        return;
    }
    if (mapping->mapping != NULL) {
        bundlefs_mapping_t bundlefs_mapping = {
            .data = mapping->asset.data,
            .mapping = mapping->mapping,
            .size = mapping->asset.size,
            .mapping_handle = mapping->mapping_handle,
        };
        bundlefs_munmap(&bundlefs_mapping);
    }
    memset(mapping, 0, sizeof(*mapping));
}

bool micropixel_open_aot_package(const bundlefs_file_t* file, micropixel_aot_package_t* package_out) {
    if (package_out == NULL) {
        return false;
    }
    memset(package_out, 0, sizeof(*package_out));
    micropixel_bundle_header_t header;
    if (!read_valid_header(file, &header, NULL)) {
        return false;
    }

    bundlefs_mapping_t bundlefs_mapping;
    if (bundlefs_mmap(file, 0U, header.bundle_size, &bundlefs_mapping) != BUNDLEFS_OK) {
        return false;
    }
    const uint8_t* bundle_bytes = bundlefs_mapping.data;
    const micropixel_bundle_header_t* mapped_header = (const micropixel_bundle_header_t*)bundle_bytes;
    if (memcmp(mapped_header, &header, sizeof(header)) != 0) {
        bundlefs_munmap(&bundlefs_mapping);
        return false;
    }

    const micropixel_bundle_section_t* sections =
        (const micropixel_bundle_section_t*)(bundle_bytes + mapped_header->toc_offset);
    const micropixel_bundle_section_t* aot_section = NULL;
    bool launch_found = mapped_header->launch_asset_id == 0U;
    bool metadata_found = false;
    for (uint32_t index = 0U; index < mapped_header->section_count; ++index) {
        const micropixel_bundle_section_t* section = &sections[index];
        const uint32_t toc_end = mapped_header->toc_offset + mapped_header->section_count * sizeof(*section);
        if (section->size == 0U || section->offset < toc_end || (section->offset & 63U) != 0U ||
            section->offset > mapped_header->bundle_size ||
            section->size > mapped_header->bundle_size - section->offset || section->flags != 0U ||
            section->reserved0 != 0U || section->reserved1 != 0U) {
            bundlefs_munmap(&bundlefs_mapping);
            return false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            const micropixel_bundle_section_t* other = &sections[previous];
            const uint64_t section_end = (uint64_t)section->offset + section->size;
            const uint64_t other_end = (uint64_t)other->offset + other->size;
            if ((uint64_t)section->offset < other_end && (uint64_t)other->offset < section_end) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
        }
        const uint8_t* section_data = bundle_bytes + section->offset;
        if (fnv1a32(section_data, section->size) != section->hash) {
            bundlefs_munmap(&bundlefs_mapping);
            return false;
        }
        if (section->kind == MICROPIXEL_BUNDLE_SECTION_AOT) {
            if (aot_section != NULL || section->id != 0U ||
                section->format != MICROPIXEL_BUNDLE_FORMAT_AOT_RELOCATABLE || section->width != 0U ||
                section->height != 0U || section->stride != 0U) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
            aot_section = section;
        } else if (section->kind == MICROPIXEL_BUNDLE_SECTION_ASSET) {
            if (section->id == 0U || section->format < MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888 ||
                section->format > MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888 || section->width == 0U ||
                section->height == 0U ||
                (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888 &&
                 (section->stride != section->width * 3U ||
                  (uint64_t)section->stride * section->height != section->size)) ||
                (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888 &&
                 (section->stride != section->width * 4U ||
                  (uint64_t)section->stride * section->height != section->size))) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
            for (uint32_t previous = 0U; previous < index; ++previous) {
                if (sections[previous].kind == MICROPIXEL_BUNDLE_SECTION_ASSET &&
                    sections[previous].id == section->id) {
                    bundlefs_munmap(&bundlefs_mapping);
                    return false;
                }
            }
            if (section->id == mapped_header->launch_asset_id) {
                if (!valid_launch_asset_section(section)) {
                    bundlefs_munmap(&bundlefs_mapping);
                    return false;
                }
                launch_found = true;
            }
        } else if (section->kind == MICROPIXEL_BUNDLE_SECTION_APP_METADATA) {
            if (metadata_found || section->id != 0U || section->size > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH ||
                section->format != MICROPIXEL_BUNDLE_FORMAT_UTF8 || section->width != 0U || section->height != 0U ||
                section->stride != 0U || !valid_display_name(section_data, section->size)) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
            metadata_found = true;
        } else {
            bundlefs_munmap(&bundlefs_mapping);
            return false;
        }
    }
    if (aot_section == NULL || !launch_found || !metadata_found) {
        bundlefs_munmap(&bundlefs_mapping);
        return false;
    }

#if CONFIG_MICROPIXEL_AOT_PACKAGE_BUFFER_IN_PSRAM
    const uint32_t payload_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    const uint32_t payload_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    uint8_t* payload = heap_caps_malloc(aot_section->size, payload_caps);
    if (payload == NULL) {
        bundlefs_munmap(&bundlefs_mapping);
        return false;
    }
    memcpy(payload, bundle_bytes + aot_section->offset, aot_section->size);
    if (wasm_runtime_is_xip_file(payload, aot_section->size)) {
        free(payload);
        bundlefs_munmap(&bundlefs_mapping);
        return false;
    }

    package_out->payload = payload;
    package_out->payload_size = aot_section->size;
    package_out->bundle_mapping = bundle_bytes;
    package_out->bundle_size = mapped_header->bundle_size;
    package_out->sections = sections;
    package_out->section_count = mapped_header->section_count;
    package_out->launch_asset_id = mapped_header->launch_asset_id;
    memcpy(package_out->app_id, mapped_header->app_id, mapped_header->app_id_length);
    package_out->mapping_handle = bundlefs_mapping.mapping_handle;
    ESP_LOGI(TAG, "Bundle mapped: app=%s virtual=%p bytes=%" PRIu32, package_out->app_id, bundle_bytes,
             mapped_header->bundle_size);
    return true;
}

void micropixel_close_aot_package(micropixel_aot_package_t* package) {
    if (package == NULL) {
        return;
    }
    if (package->payload != NULL) {
        free((void*)package->payload);
    }
    if (package->bundle_mapping != NULL) {
        bundlefs_mapping_t bundlefs_mapping = {
            .data = package->bundle_mapping,
            .mapping = package->bundle_mapping,
            .size = package->bundle_size,
            .mapping_handle = package->mapping_handle,
        };
        bundlefs_munmap(&bundlefs_mapping);
    }
    memset(package, 0, sizeof(*package));
}

bool micropixel_bundle_find_asset(const micropixel_aot_package_t* package, uint32_t asset_id,
                                  micropixel_bundle_asset_view_t* view_out) {
    if (package == NULL || view_out == NULL || package->bundle_mapping == NULL || asset_id == 0U) {
        return false;
    }
    memset(view_out, 0, sizeof(*view_out));
    for (uint32_t index = 0U; index < package->section_count; ++index) {
        const micropixel_bundle_section_t* section = &package->sections[index];
        if (section->kind == MICROPIXEL_BUNDLE_SECTION_ASSET && section->id == asset_id) {
            view_out->data = package->bundle_mapping + section->offset;
            view_out->size = section->size;
            view_out->format = section->format;
            view_out->width = section->width;
            view_out->height = section->height;
            view_out->stride = section->stride;
            view_out->content_hash = section->hash;
            return true;
        }
    }
    return false;
}
