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

/*
 * Memory-backed Bundle source. The mappable variant hands out pointers into
 * the loaded file like BundleFS on NOR; the read-only variant has no `map`
 * so the reader must serve each opened section from its own RAM copy.
 */
static const uint32_t kTestSourceToken = 0x5eedU;

static bool test_source_size(const micropixel_bundle_source_t* source, uint32_t* size_out) {
    if (source->state[0] != kTestSourceToken) {
        return false;
    }
    *size_out = test_bundle_size;
    return true;
}

static bool test_source_read(const micropixel_bundle_source_t* source, uint32_t offset, void* destination,
                             uint32_t size) {
    if (source->state[0] != kTestSourceToken || offset > test_bundle_size || size > test_bundle_size - offset) {
        return false;
    }
    memcpy(destination, test_bundle + offset, size);
    return true;
}

static void test_source_unmap(micropixel_bundle_mapping_t* mapping) {
    if (mapping->base != NULL && active_mappings > 0U) {
        --active_mappings;
    }
    memset(mapping, 0, sizeof(*mapping));
}

static const micropixel_bundle_mapping_ops_t kTestMappingOps = {.unmap = test_source_unmap};

static bool test_source_map(const micropixel_bundle_source_t* source, uint32_t offset, uint32_t size,
                            micropixel_bundle_mapping_t* mapping_out) {
    if (source->state[0] != kTestSourceToken || size == 0U || offset > test_bundle_size ||
        size > test_bundle_size - offset) {
        return false;
    }
    *mapping_out = (micropixel_bundle_mapping_t){
        .data = test_bundle + offset,
        .base = test_bundle + offset,
        .size = size,
        .handle = next_mapping_handle++,
        .ops = &kTestMappingOps,
    };
    ++active_mappings;
    last_mapping_offset = offset;
    last_mapping_size = size;
    return true;
}

static const micropixel_bundle_source_ops_t kMappableSourceOps = {
    .size = test_source_size,
    .read = test_source_read,
    .map = test_source_map,
};

static const micropixel_bundle_source_ops_t kReadOnlySourceOps = {
    .size = test_source_size,
    .read = test_source_read,
    .map = NULL,
};

static bool in_test_bundle(const uint8_t* pointer) {
    return pointer != NULL && pointer >= test_bundle && pointer < test_bundle + test_bundle_size;
}

static bool launch_asset_offset(const micropixel_bundle_header_t* header, uint32_t* offset_out) {
    for (uint32_t index = 0U; index < header->section_count; ++index) {
        micropixel_bundle_section_t section;
        memcpy(&section, test_bundle + header->toc_offset + index * sizeof(section), sizeof(section));
        if (section.kind == MICROPIXEL_BUNDLE_SECTION_ASSET && section.id == header->launch_asset_id) {
            *offset_out = section.offset;
            return true;
        }
    }
    return false;
}

static bool validate_bundle(bool mappable) {
    micropixel_bundle_source_t file = {.ops = mappable ? &kMappableSourceOps : &kReadOnlySourceOps};
    file.state[0] = kTestSourceToken;
    micropixel_bundle_header_t header;
    memcpy(&header, test_bundle, sizeof(header));
    const bool has_launch_asset = header.launch_asset_id != 0U;
    micropixel_bundle_metadata_t metadata;
    const char* test_locale = getenv("MICROPIXEL_TEST_LOCALE");
    const bool metadata_valid = test_locale == NULL
                                    ? micropixel_read_bundle_metadata(&file, &metadata)
                                    : micropixel_read_bundle_metadata_for_locale(&file, test_locale, &metadata);
    if (getenv("MICROPIXEL_EXPECT_INVALID_METADATA") != NULL) {
        return check(!metadata_valid, "invalid App version must be rejected") &&
               check(active_mappings == 0U, "invalid metadata must release mappings");
    }
    const char* expected_name = getenv("MICROPIXEL_EXPECT_DISPLAY_NAME");
    const char* expected_version = getenv("MICROPIXEL_EXPECT_METADATA_SCHEMA");
    const char* expected_type = getenv("MICROPIXEL_EXPECT_PACKAGE_TYPE");
    const char* expected_package_version = getenv("MICROPIXEL_EXPECT_PACKAGE_VERSION");
    if (!check(metadata_valid, "Bundle metadata must validate") ||
        !check(metadata.bundle_size == test_bundle_size, "metadata must expose the Bundle logical size") ||
        !check(expected_package_version == NULL ||
                   strcmp((const char*)metadata.package_version, expected_package_version) == 0,
               "metadata must expose the expected package version") ||
        !check(metadata.app_id[0] != 0U && metadata.display_name[0] != 0U, "metadata must contain App identity") ||
        !check(expected_name == NULL || strcmp((const char*)metadata.display_name, expected_name) == 0,
               "metadata must select the expected localized display name") ||
        !check(expected_version == NULL ||
                   metadata.metadata_schema_version == (uint32_t)strtoul(expected_version, NULL, 10),
               "metadata must expose the expected metadata schema") ||
        !check(expected_type == NULL || (strcmp(expected_type, "component") == 0
                                             ? metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT
                                             : metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_APP),
               "metadata must expose the expected package type")) {
        return false;
    }

    if (metadata.package_type == MICROPIXEL_BUNDLE_PACKAGE_COMPONENT) {
        micropixel_bundle_metadata_t validated;
        if (!check(!has_launch_asset, "Component Package must not have a launch asset") ||
            !check(micropixel_validate_component_package(&file, &validated),
                   "font Component Package must pass semantic validation") ||
            !check(validated.component_type == MICROPIXEL_BUNDLE_COMPONENT_FONT && validated.language_count > 0U,
                   "validated font Component must expose its type and languages")) {
            return false;
        }
        micropixel_aot_package_t component_aot;
        return check(!micropixel_open_aot_package(&file, &component_aot),
                     "Component Package must never open as executable AOT") &&
               check(active_mappings == 0U, "Component validation must release its Bundle mapping");
    }

    /* A mappable source hands out one mapping per view; a read-only source never maps. */
    const uint32_t one_view = mappable ? 1U : 0U;
    micropixel_bundle_asset_mapping_t cover;
    if (has_launch_asset) {
        if (!check(micropixel_open_launch_asset(&file, &cover), "launch cover must open") ||
            !check(active_mappings == one_view, "cover path must own exactly one mapping when mappable") ||
            !check(!mappable || last_mapping_offset > 0U, "cover mapping must begin at the asset, not Bundle start") ||
            !check(!mappable || (last_mapping_size == cover.asset.size && last_mapping_size < metadata.bundle_size),
                   "Hall must map only the launch asset") ||
            !check(cover.asset.data != NULL && cover.mapping.data == cover.asset.data,
                   "cover view must expose the asset bytes") ||
            !check(mappable == in_test_bundle(cover.asset.data),
                   "mappable covers alias the source; read-only covers are Host-owned copies") ||
            !check(cover.asset.format == MICROPIXEL_BUNDLE_FORMAT_JPEG ||
                       cover.asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG,
                   "production Hall cover must use JPEG or PNG") ||
            !check(cover.asset.content_hash != 0U, "Hall cover identity must include its content hash")) {
            return false;
        }
        if (!mappable) {
            /* Hall's NAND copy must survive an App session without reopening its source. */
            const uint8_t* retained_data = cover.asset.data;
            micropixel_aot_package_t retained_package;
            micropixel_bundle_asset_mapping_t guest_cover;
            if (!check(micropixel_open_aot_package(&file, &retained_package),
                       "App must open while the Hall RAM cover is retained") ||
                !check(micropixel_bundle_open_asset(&retained_package, retained_package.launch_asset_id, &guest_cover),
                       "Guest must open its own cover while Hall retains a copy") ||
                !check(guest_cover.asset.data != retained_data && guest_cover.asset.size == cover.asset.size &&
                           memcmp(guest_cover.asset.data, retained_data, cover.asset.size) == 0,
                       "Guest and Hall must own independent copies of the same cover")) {
                return false;
            }
            micropixel_close_asset_mapping(&guest_cover);
            micropixel_close_aot_package(&retained_package);
            uint32_t retained_offset = 0U;
            if (!check(launch_asset_offset(&header, &retained_offset), "retained cover must have a source offset") ||
                !check(cover.asset.data == retained_data &&
                           memcmp(retained_data, test_bundle + retained_offset, cover.asset.size) == 0,
                       "App teardown must preserve the Hall cover for immediate reuse")) {
                return false;
            }
        }
        micropixel_close_asset_mapping(&cover);
        if (!check(active_mappings == 0U, "closing a cover must release its mapping")) {
            return false;
        }
    } else if (!check(!micropixel_open_launch_asset(&file, &cover),
                      "a Bundle without a launch asset must select the Hall fallback cover") ||
               !check(active_mappings == 0U, "missing cover lookup must not create a mapping")) {
        return false;
    }

    micropixel_aot_package_t package;
    if (!check(micropixel_open_aot_package(&file, &package), "Bundle must open as an AOT package") ||
        !check(active_mappings == 0U, "an open package must not keep any part of the Bundle mapped") ||
        !check(package.sections != NULL && package.section_count == header.section_count &&
                   !in_test_bundle((const uint8_t*)package.sections),
               "the package must own a Host copy of the TOC") ||
        !check(package.payload != NULL && package.payload_size > 0U && !in_test_bundle(package.payload),
               "relocatable AOT must be copied for WAMR") ||
        !check((package.aot_flags & MICROPIXEL_BUNDLE_AOT_FLAG_THREADING_DECLARED) != 0U,
               "current App Bundles must declare their Guest threading policy")) {
        return false;
    }
    micropixel_bundle_asset_mapping_t launch;
    const bool opened_launch = micropixel_bundle_open_asset(&package, package.launch_asset_id, &launch);
    if (!check(opened_launch == has_launch_asset, "running package launch asset state must match its header")) {
        return false;
    }
    uint32_t cover_offset = 0U;
    if (has_launch_asset) {
        if (!check(launch_asset_offset(&header, &cover_offset), "launch asset must be listed in the TOC") ||
            !check(active_mappings == one_view, "an opened section must own exactly one mapping when mappable") ||
            !check(!mappable || (last_mapping_offset == cover_offset && last_mapping_size == launch.asset.size),
                   "an opened section must map only its own bytes") ||
            !check(mappable == in_test_bundle(launch.asset.data),
                   "mappable sections alias the source; read-only sections are Host-owned copies") ||
            !check(launch.asset.content_hash != 0U && launch.asset.format != 0U,
                   "an opened section must carry its TOC attributes")) {
            return false;
        }
        micropixel_close_asset_mapping(&launch);
        if (!check(active_mappings == 0U, "closing a section must release its mapping")) {
            return false;
        }
        /* Corruption inside a section is caught when that section is opened, without touching others. */
        const uint8_t original = test_bundle[cover_offset];
        test_bundle[cover_offset] ^= 0xffU;
        const bool corrupt_opened = micropixel_bundle_open_asset(&package, package.launch_asset_id, &launch);
        if (corrupt_opened) {
            micropixel_close_asset_mapping(&launch);
        }
        micropixel_bundle_metadata_t validated;
        const bool corrupt_validated = micropixel_validate_app_package(&file, &validated);
        test_bundle[cover_offset] = original;
        if (!check(!corrupt_opened, "a section whose hash mismatches must not open") ||
            !check(!corrupt_validated, "install validation must reject a Bundle with a corrupt section") ||
            !check(active_mappings == 0U, "failed section validation must not leak a mapping")) {
            return false;
        }
    }
    micropixel_bundle_asset_mapping_t missing;
    micropixel_bundle_font_mapping_t missing_font;
    if (!check(!micropixel_bundle_open_asset(&package, 0xfffffffeU, &missing), "unknown asset ids must not open") ||
        !check(!micropixel_bundle_open_font(&package, 0xfffffffeU, &missing_font), "unknown font ids must not open") ||
        !check(active_mappings == 0U, "failed lookups must not create mappings")) {
        return false;
    }
    micropixel_close_aot_package(&package);
    if (!check(package.sections == NULL && package.payload == NULL, "closing a package must clear it") ||
        !check(active_mappings == 0U, "closing a package must leave no mapping behind")) {
        return false;
    }

    micropixel_bundle_metadata_t validated;
    if (!check(micropixel_validate_app_package(&file, &validated), "install validation must accept a valid Bundle") ||
        !check(validated.bundle_size == test_bundle_size && validated.app_id[0] != 0U,
               "install validation must return the Bundle metadata") ||
        !check(active_mappings == 0U, "install validation must stream and leave no mapping behind")) {
        return false;
    }
    if (!has_launch_asset) {
        return true;
    }
    micropixel_bundle_asset_mapping_t cover_again;
    const uint8_t original = test_bundle[cover_offset];
    test_bundle[cover_offset] ^= 0xffU;
    const bool corrupt_cover_opened = micropixel_open_launch_asset(&file, &cover_again);
    test_bundle[cover_offset] = original;
    if (corrupt_cover_opened) {
        micropixel_close_asset_mapping(&cover_again);
    }
    return check(!corrupt_cover_opened, "Bundle reader must reject a corrupt cover") &&
           check(active_mappings == 0U, "failed cover validation must not leak a mapping");
}

int main(int argc, char** argv) {
    if (argc != 2 || !load_bundle(argv[1])) {
        fprintf(stderr, "Usage: bundle_reader_test APP_BUNDLE\n");
        return 2;
    }
    const bool passed = validate_bundle(true) && validate_bundle(false);
    free(test_bundle);
    if (!passed) {
        return 1;
    }
    puts(
        "Bundle reader host integration passed (mappable and read-only Bundle sources, on-demand sections, install "
        "validation and corruption checks).");
    return 0;
}
