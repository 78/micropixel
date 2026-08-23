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
#include "esp_partition.h"
#include "sdkconfig.h"
#include "wasm_export.h"
#define APP_STORE_PARTITION_SUBTYPE 0x40

static const char* TAG = "micropixel_package";

static uint32_t fnv1a32(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261U;

    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
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
    if (bytes == NULL || length == 0U || length > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH) {
        return false;
    }
    if (bytes[0] == ' ' || bytes[length - 1U] == ' ') {
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
    if (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_RGB888) {
        return section->stride == section->width * 3U && (uint64_t)section->stride * section->height == section->size;
    }
    return section->format == MICROPIXEL_BUNDLE_FORMAT_PNG && section->stride == 0U;
}

static const esp_partition_t* find_app_store(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, APP_STORE_PARTITION_SUBTYPE, "app_store");
}

static bool erased_header(const micropixel_bundle_header_t* header) {
    const uint8_t* bytes = (const uint8_t*)header;
    for (size_t index = 0U; index < sizeof(*header); ++index) {
        if (bytes[index] != UINT8_MAX) {
            return false;
        }
    }
    return true;
}

static bool read_display_name(const esp_partition_t* partition, uint32_t store_offset,
                              const micropixel_bundle_header_t* header, uint8_t* display_name_out) {
    const uint32_t toc_end = header->toc_offset + header->section_count * sizeof(micropixel_bundle_section_t);
    bool metadata_found = false;
    uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U] = {0};
    for (uint32_t index = 0U; index < header->section_count; ++index) {
        micropixel_bundle_section_t section;
        const uint32_t section_offset = store_offset + header->toc_offset + index * sizeof(section);
        if (esp_partition_read(partition, section_offset, &section, sizeof(section)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to read Bundle section #%" PRIu32 " for App metadata", index);
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
            esp_partition_read(partition, store_offset + section.offset, display_name, section.size) != ESP_OK ||
            fnv1a32(display_name, section.size) != section.hash || !valid_display_name(display_name, section.size)) {
            ESP_LOGE(TAG, "invalid or duplicate App metadata in Bundle at offset=0x%" PRIx32, store_offset);
            return false;
        }
        metadata_found = true;
    }
    if (!metadata_found) {
        ESP_LOGE(TAG, "Bundle at offset=0x%" PRIx32 " has no App metadata", store_offset);
        return false;
    }
    if (display_name_out != NULL) {
        memcpy(display_name_out, display_name, sizeof(display_name));
    }
    return true;
}

static bool read_valid_header(const esp_partition_t* partition, uint32_t store_offset,
                              micropixel_bundle_header_t* header_out, uint8_t* display_name_out) {
    if (partition == NULL || header_out == NULL || (store_offset % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT) != 0U ||
        store_offset > partition->size || sizeof(*header_out) > partition->size - store_offset) {
        ESP_LOGE(TAG, "invalid App Store extent offset=0x%" PRIx32, store_offset);
        return false;
    }
    micropixel_bundle_header_t header;
    if (esp_partition_read(partition, store_offset, &header, sizeof(header)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read Bundle header at App Store offset=0x%" PRIx32, store_offset);
        return false;
    }
    if (memcmp(header.magic, MICROPIXEL_BUNDLE_MAGIC, sizeof(header.magic)) != 0) {
        ESP_LOGE(TAG, "no valid Bundle v1 header at App Store offset=0x%" PRIx32, store_offset);
        return false;
    }
    if (header.version != MICROPIXEL_BUNDLE_VERSION || header.header_size != sizeof(header)) {
        ESP_LOGE(TAG, "unsupported Bundle header: version=%" PRIu32 " size=%" PRIu32, header.version,
                 header.header_size);
        return false;
    }
    if (header.framework_abi_version != MICROPIXEL_BUNDLE_FRAMEWORK_ABI_VERSION) {
        ESP_LOGE(TAG, "unsupported framework ABI: %" PRIu32, header.framework_abi_version);
        return false;
    }
    if (header.bundle_size == 0U || header.bundle_size > partition->size - store_offset ||
        (header.bundle_size % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT) != 0U || header.section_count == 0U ||
        header.section_count > MICROPIXEL_BUNDLE_MAX_SECTIONS || header.toc_offset != sizeof(header) ||
        header.section_count > (header.bundle_size - header.toc_offset) / sizeof(micropixel_bundle_section_t) ||
        !valid_app_id(&header) || header.reserved0 != 0U || header.reserved1 != 0U || header.reserved2 != 0U ||
        header.reserved3 != 0U || header.reserved4 != 0U) {
        ESP_LOGE(TAG, "invalid Bundle v1 bounds, AppId or reserved fields at App Store offset=0x%" PRIx32,
                 store_offset);
        return false;
    }
    micropixel_bundle_header_t hash_header = header;
    uint32_t expected_header_hash = hash_header.header_hash;
    hash_header.header_hash = 0U;
    if (fnv1a32((const uint8_t*)&hash_header, sizeof(hash_header)) != expected_header_hash) {
        ESP_LOGE(TAG, "Bundle header hash mismatch at App Store offset=0x%" PRIx32, store_offset);
        return false;
    }
    if (!read_display_name(partition, store_offset, &header, display_name_out)) {
        return false;
    }
    *header_out = header;
    return true;
}

bool micropixel_scan_app_store(micropixel_bundle_metadata_t* apps_out, uint32_t capacity, uint32_t* count_out) {
    if (apps_out == NULL || capacity == 0U || count_out == NULL) {
        return false;
    }
    *count_out = 0U;
    const esp_partition_t* partition = find_app_store();
    if (partition == NULL) {
        ESP_LOGE(TAG, "partition 'app_store' was not found");
        return false;
    }
    ESP_LOGI(TAG, "found App Store at 0x%" PRIx32 ", size=%" PRIu32, partition->address, partition->size);

    uint32_t store_offset = 0U;
    while (store_offset <= partition->size - sizeof(micropixel_bundle_header_t) && *count_out < capacity) {
        micropixel_bundle_header_t header;
        if (esp_partition_read(partition, store_offset, &header, sizeof(header)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to scan App Store offset=0x%" PRIx32, store_offset);
            return false;
        }
        if (erased_header(&header)) {
            break;
        }
        uint8_t display_name[MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U] = {0};
        if (!read_valid_header(partition, store_offset, &header, display_name)) {
            return false;
        }

        micropixel_bundle_metadata_t* metadata = &apps_out[*count_out];
        memset(metadata, 0, sizeof(*metadata));
        metadata->store_offset = store_offset;
        metadata->bundle_size = header.bundle_size;
        memcpy(metadata->app_id, header.app_id, header.app_id_length);
        memcpy(metadata->display_name, display_name, sizeof(metadata->display_name));
        ++*count_out;
        store_offset += header.bundle_size;
    }
    ESP_LOGI(TAG, "App Store catalog: apps=%" PRIu32, *count_out);
    return true;
}

bool micropixel_open_launch_asset(uint32_t store_offset, micropixel_bundle_asset_mapping_t* mapping_out) {
    if (mapping_out == NULL) {
        return false;
    }
    memset(mapping_out, 0, sizeof(*mapping_out));

    const esp_partition_t* partition = find_app_store();
    if (partition == NULL) {
        ESP_LOGE(TAG, "partition 'app_store' was not found");
        return false;
    }
    micropixel_bundle_header_t header;
    if (!read_valid_header(partition, store_offset, &header, NULL) || header.launch_asset_id == 0U) {
        ESP_LOGW(TAG, "Bundle at App Store offset=0x%" PRIx32 " has no launch asset", store_offset);
        return false;
    }

    const uint32_t toc_end = header.toc_offset + header.section_count * sizeof(micropixel_bundle_section_t);
    micropixel_bundle_section_t launch_section;
    memset(&launch_section, 0, sizeof(launch_section));
    bool launch_found = false;
    for (uint32_t index = 0U; index < header.section_count; ++index) {
        micropixel_bundle_section_t section;
        const uint32_t section_offset = store_offset + header.toc_offset + index * sizeof(micropixel_bundle_section_t);
        if (esp_partition_read(partition, section_offset, &section, sizeof(section)) != ESP_OK) {
            ESP_LOGE(TAG, "failed to read Bundle section #%" PRIu32 " for Hall cover", index);
            return false;
        }
        if (section.kind != MICROPIXEL_BUNDLE_SECTION_ASSET || section.id != header.launch_asset_id) {
            continue;
        }
        if (launch_found || section.size == 0U || section.offset < toc_end || (section.offset & 63U) != 0U ||
            section.offset > header.bundle_size || section.size > header.bundle_size - section.offset ||
            !valid_launch_asset_section(&section) || section.flags != 0U || section.reserved0 != 0U ||
            section.reserved1 != 0U) {
            ESP_LOGE(TAG, "invalid or duplicate launch asset in Bundle at offset=0x%" PRIx32, store_offset);
            return false;
        }
        launch_section = section;
        launch_found = true;
    }
    if (!launch_found) {
        ESP_LOGE(TAG, "declared launch asset was not found at App Store offset=0x%" PRIx32, store_offset);
        return false;
    }

    const void* mapping = NULL;
    esp_partition_mmap_handle_t mapping_handle = 0;
    esp_err_t map_error = esp_partition_mmap(partition, store_offset + launch_section.offset, launch_section.size,
                                             ESP_PARTITION_MMAP_DATA, &mapping, &mapping_handle);
    if (map_error != ESP_OK) {
        ESP_LOGE(TAG, "unable to mmap Hall cover: %s", esp_err_to_name(map_error));
        return false;
    }
    if (fnv1a32(mapping, launch_section.size) != launch_section.hash) {
        ESP_LOGE(TAG, "Hall cover hash mismatch at App Store offset=0x%" PRIx32, store_offset);
        esp_partition_munmap(mapping_handle);
        return false;
    }

    mapping_out->asset.data = mapping;
    mapping_out->asset.size = launch_section.size;
    mapping_out->asset.format = launch_section.format;
    mapping_out->asset.width = launch_section.width;
    mapping_out->asset.height = launch_section.height;
    mapping_out->asset.stride = launch_section.stride;
    mapping_out->asset.content_hash = launch_section.hash;
    mapping_out->mapping = mapping;
    mapping_out->mapping_handle = mapping_handle;
    ESP_LOGI(TAG,
             "Hall cover mapped read-only: store-offset=0x%" PRIx32 " asset-offset=0x%" PRIx32
             " virtual=%p bytes=%" PRIu32 " size=%" PRIu32 "x%" PRIu32 " format=%" PRIu32,
             store_offset, launch_section.offset, mapping, launch_section.size, launch_section.width,
             launch_section.height, launch_section.format);
    return true;
}

void micropixel_close_asset_mapping(micropixel_bundle_asset_mapping_t* mapping) {
    if (mapping == NULL) {
        return;
    }
    if (mapping->mapping != NULL) {
        esp_partition_munmap(mapping->mapping_handle);
        ESP_LOGI(TAG, "released read-only Hall cover mapping");
    }
    memset(mapping, 0, sizeof(*mapping));
}

bool micropixel_open_aot_package(uint32_t store_offset, micropixel_aot_package_t* package_out) {
    if (package_out == NULL) {
        return false;
    }
    memset(package_out, 0, sizeof(*package_out));

    const esp_partition_t* partition = find_app_store();
    if (partition == NULL) {
        ESP_LOGE(TAG, "partition 'app_store' was not found");
        return false;
    }
    micropixel_bundle_header_t header;
    if (!read_valid_header(partition, store_offset, &header, NULL)) {
        return false;
    }

    const void* mapping = NULL;
    esp_partition_mmap_handle_t mapping_handle = 0;
    esp_err_t map_error = esp_partition_mmap(partition, store_offset, header.bundle_size, ESP_PARTITION_MMAP_DATA,
                                             &mapping, &mapping_handle);
    if (map_error != ESP_OK) {
        ESP_LOGE(TAG, "unable to mmap Bundle: %s", esp_err_to_name(map_error));
        return false;
    }

    const micropixel_bundle_header_t* mapped_header = mapping;
    if (memcmp(mapped_header, &header, sizeof(header)) != 0) {
        ESP_LOGE(TAG, "Bundle header changed while mapping");
        esp_partition_munmap(mapping_handle);
        return false;
    }

    const uint8_t* bundle_bytes = mapping;
    const micropixel_bundle_section_t* sections =
        (const micropixel_bundle_section_t*)(bundle_bytes + mapped_header->toc_offset);
    const micropixel_bundle_section_t* aot_section = NULL;
    bool launch_found = mapped_header->launch_asset_id == 0U;
    bool metadata_found = false;
    for (uint32_t index = 0U; index < mapped_header->section_count; ++index) {
        const micropixel_bundle_section_t* section = &sections[index];
        uint32_t toc_end = mapped_header->toc_offset + mapped_header->section_count * sizeof(*section);
        if (section->size == 0U || section->offset < toc_end || (section->offset & 63U) != 0U ||
            section->offset > mapped_header->bundle_size ||
            section->size > mapped_header->bundle_size - section->offset || section->flags != 0U ||
            section->reserved0 != 0U || section->reserved1 != 0U) {
            ESP_LOGE(TAG, "invalid Bundle section #%" PRIu32, index);
            esp_partition_munmap(mapping_handle);
            return false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            const micropixel_bundle_section_t* other = &sections[previous];
            uint64_t section_end = (uint64_t)section->offset + section->size;
            uint64_t other_end = (uint64_t)other->offset + other->size;
            if ((uint64_t)section->offset < other_end && (uint64_t)other->offset < section_end) {
                ESP_LOGE(TAG, "overlapping Bundle sections #%" PRIu32 " and #%" PRIu32, previous, index);
                esp_partition_munmap(mapping_handle);
                return false;
            }
        }
        const uint8_t* section_data = bundle_bytes + section->offset;
        if (fnv1a32(section_data, section->size) != section->hash) {
            ESP_LOGE(TAG, "Bundle section #%" PRIu32 " hash mismatch", index);
            esp_partition_munmap(mapping_handle);
            return false;
        }
        if (section->kind == MICROPIXEL_BUNDLE_SECTION_AOT) {
            if (aot_section != NULL || section->id != 0U ||
                section->format != MICROPIXEL_BUNDLE_FORMAT_AOT_RELOCATABLE || section->width != 0U ||
                section->height != 0U || section->stride != 0U) {
                ESP_LOGE(TAG, "invalid or duplicate relocatable AOT section");
                esp_partition_munmap(mapping_handle);
                return false;
            }
            aot_section = section;
        } else if (section->kind == MICROPIXEL_BUNDLE_SECTION_ASSET) {
            if (section->id == 0U || section->format < MICROPIXEL_BUNDLE_FORMAT_RAW_RGB888 ||
                section->format > MICROPIXEL_BUNDLE_FORMAT_RAW_ARGB8888 || section->width == 0U ||
                section->height == 0U ||
                (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_RGB888 &&
                 (section->stride != section->width * 3U ||
                  (uint64_t)section->stride * section->height != section->size)) ||
                (section->format == MICROPIXEL_BUNDLE_FORMAT_RAW_ARGB8888 &&
                 (section->stride != section->width * 4U ||
                  (uint64_t)section->stride * section->height != section->size))) {
                ESP_LOGE(TAG, "invalid asset section #%" PRIu32, index);
                esp_partition_munmap(mapping_handle);
                return false;
            }
            for (uint32_t previous = 0U; previous < index; ++previous) {
                if (sections[previous].kind == MICROPIXEL_BUNDLE_SECTION_ASSET &&
                    sections[previous].id == section->id) {
                    ESP_LOGE(TAG, "duplicate asset id=%" PRIu32, section->id);
                    esp_partition_munmap(mapping_handle);
                    return false;
                }
            }
            if (section->id == mapped_header->launch_asset_id) {
                if (!valid_launch_asset_section(section)) {
                    ESP_LOGE(TAG, "launch asset must be raw RGB888 or PNG");
                    esp_partition_munmap(mapping_handle);
                    return false;
                }
                launch_found = true;
            }
        } else if (section->kind == MICROPIXEL_BUNDLE_SECTION_APP_METADATA) {
            if (metadata_found || section->id != 0U || section->size > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH ||
                section->format != MICROPIXEL_BUNDLE_FORMAT_UTF8 || section->width != 0U || section->height != 0U ||
                section->stride != 0U || !valid_display_name(section_data, section->size)) {
                ESP_LOGE(TAG, "invalid or duplicate App metadata section");
                esp_partition_munmap(mapping_handle);
                return false;
            }
            metadata_found = true;
        } else {
            ESP_LOGE(TAG, "unknown Bundle section kind=%" PRIu32, section->kind);
            esp_partition_munmap(mapping_handle);
            return false;
        }
    }
    if (aot_section == NULL || !launch_found || !metadata_found) {
        ESP_LOGE(TAG, "Bundle is missing AOT, declared launch asset or App metadata");
        esp_partition_munmap(mapping_handle);
        return false;
    }

#if CONFIG_MICROPIXEL_AOT_PACKAGE_BUFFER_IN_PSRAM
    const uint32_t payload_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    const uint32_t payload_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    uint8_t* payload = heap_caps_malloc(aot_section->size, payload_caps);
    if (payload == NULL) {
        ESP_LOGE(TAG, "unable to allocate %" PRIu32 " bytes for AOT", aot_section->size);
        esp_partition_munmap(mapping_handle);
        return false;
    }
    memcpy(payload, bundle_bytes + aot_section->offset, aot_section->size);
    bool is_xip = wasm_runtime_is_xip_file(payload, aot_section->size);
    if (is_xip) {
        ESP_LOGE(TAG, "Bundle v1 product path accepts only relocatable AOT");
        free(payload);
        esp_partition_munmap(mapping_handle);
        return false;
    }

    char app_id[MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U] = {0};
    memcpy(app_id, mapped_header->app_id, mapped_header->app_id_length);
    ESP_LOGI(TAG,
             "Bundle mapped read-only: app=%s store-offset=0x%" PRIx32 " virtual=%p bytes=%" PRIu32 " sections=%" PRIu32
             " launch=%" PRIu32,
             app_id, store_offset, mapping, mapped_header->bundle_size, mapped_header->section_count,
             mapped_header->launch_asset_id);
    ESP_LOGI(TAG, "AOT input: copied from Bundle mmap to %s at %p, format=relocatable",
             esp_ptr_external_ram(payload) ? "PSRAM" : "internal SRAM", payload);
    ESP_LOGI(TAG, "validated AOT package: payload=%" PRIu32 ", fnv1a=%08" PRIx32, aot_section->size, aot_section->hash);
    package_out->payload = payload;
    package_out->payload_size = aot_section->size;
    package_out->bundle_mapping = mapping;
    package_out->bundle_size = mapped_header->bundle_size;
    package_out->sections = sections;
    package_out->section_count = mapped_header->section_count;
    package_out->launch_asset_id = mapped_header->launch_asset_id;
    memcpy(package_out->app_id, mapped_header->app_id, mapped_header->app_id_length);
    package_out->mapping_handle = mapping_handle;
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
        esp_partition_munmap(package->mapping_handle);
        ESP_LOGI(TAG, "released read-only Bundle mapping");
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
