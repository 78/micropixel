#include "runtime/bundle/bundle_reader.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device/font_cbin_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "psa/crypto.h"
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

typedef struct {
    const char* language;
    uint32_t language_length;
    const char* script;
    uint32_t script_length;
    const char* region;
    uint32_t region_length;
} locale_parts_t;

static bool valid_canonical_locale(const char* locale) {
    if (locale == NULL) {
        return false;
    }
    const size_t length = strnlen(locale, MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH + 1U);
    if (length < 2U || length > MICROPIXEL_BUNDLE_LOCALE_MAX_LENGTH) {
        return false;
    }
    size_t index = 0U;
    while (index < length && locale[index] != '-') {
        if (locale[index] < 'a' || locale[index] > 'z') {
            return false;
        }
        ++index;
    }
    if (index < 2U || index > 3U || index == length) {
        return index == length && index >= 2U && index <= 3U;
    }

    ++index;
    size_t subtag_start = index;
    while (index < length && locale[index] != '-') {
        ++index;
    }
    const size_t subtag_length = index - subtag_start;
    const bool script = subtag_length == 4U && locale[subtag_start] >= 'A' && locale[subtag_start] <= 'Z';
    if (script) {
        for (size_t offset = 1U; offset < 4U; ++offset) {
            if (locale[subtag_start + offset] < 'a' || locale[subtag_start + offset] > 'z') {
                return false;
            }
        }
        if (index == length) {
            return true;
        }
        ++index;
        subtag_start = index;
        while (index < length && locale[index] != '-') {
            ++index;
        }
    }
    if (index != length) {
        return false;
    }
    const size_t region_length = index - subtag_start;
    if (region_length == 2U) {
        return locale[subtag_start] >= 'A' && locale[subtag_start] <= 'Z' && locale[subtag_start + 1U] >= 'A' &&
               locale[subtag_start + 1U] <= 'Z';
    }
    return region_length == 3U && locale[subtag_start] >= '0' && locale[subtag_start] <= '9' &&
           locale[subtag_start + 1U] >= '0' && locale[subtag_start + 1U] <= '9' && locale[subtag_start + 2U] >= '0' &&
           locale[subtag_start + 2U] <= '9';
}

static locale_parts_t parse_locale_parts(const char* locale) {
    locale_parts_t result = {.language = locale};
    const char* first = strchr(locale, '-');
    result.language_length = first == NULL ? (uint32_t)strlen(locale) : (uint32_t)(first - locale);
    if (first == NULL) {
        return result;
    }
    const char* second = strchr(first + 1, '-');
    const uint32_t subtag_length = second == NULL ? (uint32_t)strlen(first + 1) : (uint32_t)(second - first - 1);
    if (subtag_length == 4U) {
        result.script = first + 1;
        result.script_length = subtag_length;
        if (second != NULL) {
            result.region = second + 1;
            result.region_length = (uint32_t)strlen(second + 1);
        }
    } else {
        result.region = first + 1;
        result.region_length = subtag_length;
    }
    return result;
}

static bool equal_locale_part(const char* left, uint32_t left_length, const char* right, uint32_t right_length) {
    return left_length == right_length && (left_length == 0U || memcmp(left, right, left_length) == 0);
}

static void likely_script(const locale_parts_t* locale, const char** script_out, uint32_t* length_out) {
    *script_out = locale->script;
    *length_out = locale->script_length;
    if (locale->script_length != 0U || locale->language_length != 2U || memcmp(locale->language, "zh", 2U) != 0 ||
        locale->region_length != 2U) {
        return;
    }
    if (memcmp(locale->region, "TW", 2U) == 0 || memcmp(locale->region, "HK", 2U) == 0 ||
        memcmp(locale->region, "MO", 2U) == 0) {
        *script_out = "Hant";
        *length_out = 4U;
    } else if (memcmp(locale->region, "CN", 2U) == 0 || memcmp(locale->region, "SG", 2U) == 0) {
        *script_out = "Hans";
        *length_out = 4U;
    }
}

static bool same_language_and_script(const char* requested, const char* available) {
    const locale_parts_t requested_parts = parse_locale_parts(requested);
    const locale_parts_t available_parts = parse_locale_parts(available);
    const char* requested_script = NULL;
    const char* available_script = NULL;
    uint32_t requested_script_length = 0U;
    uint32_t available_script_length = 0U;
    likely_script(&requested_parts, &requested_script, &requested_script_length);
    likely_script(&available_parts, &available_script, &available_script_length);
    return equal_locale_part(requested_parts.language, requested_parts.language_length, available_parts.language,
                             available_parts.language_length) &&
           requested_script_length != 0U &&
           equal_locale_part(requested_script, requested_script_length, available_script, available_script_length);
}

static bool bare_language_match(const char* requested, const char* available) {
    const locale_parts_t requested_parts = parse_locale_parts(requested);
    const locale_parts_t available_parts = parse_locale_parts(available);
    return available_parts.script_length == 0U && available_parts.region_length == 0U &&
           equal_locale_part(requested_parts.language, requested_parts.language_length, available_parts.language,
                             available_parts.language_length);
}

static bool valid_utf8_sequence(const uint8_t* bytes, uint32_t remaining, uint32_t* sequence_length_out) {
    const uint8_t first = bytes[0];
    uint32_t sequence_length = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
        sequence_length = 2U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        sequence_length = 3U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        sequence_length = 4U;
    } else {
        return false;
    }
    if (sequence_length > remaining) {
        return false;
    }
    for (uint32_t index = 1U; index < sequence_length; ++index) {
        if ((bytes[index] & 0xc0U) != 0x80U) {
            return false;
        }
    }
    if ((first == 0xe0U && bytes[1] < 0xa0U) || (first == 0xedU && bytes[1] >= 0xa0U) ||
        (first == 0xf0U && bytes[1] < 0x90U) || (first == 0xf4U && bytes[1] >= 0x90U)) {
        return false;
    }
    *sequence_length_out = sequence_length;
    return true;
}

static bool safe_json_envelope(const uint8_t* bytes, uint32_t length) {
    uint32_t nesting = 0U;
    bool in_string = false;
    bool escaped = false;
    for (uint32_t offset = 0U; offset < length;) {
        const uint8_t byte = bytes[offset];
        if (byte >= 0x80U) {
            uint32_t sequence_length = 0U;
            if (!valid_utf8_sequence(bytes + offset, length - offset, &sequence_length)) {
                return false;
            }
            offset += sequence_length;
            continue;
        }
        ++offset;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            } else if (byte < 0x20U) {
                return false;
            }
            continue;
        }
        if (byte == '"') {
            in_string = true;
        } else if (byte == '{' || byte == '[') {
            if (++nesting > 8U) {
                return false;
            }
        } else if (byte == '}' || byte == ']') {
            if (nesting == 0U) {
                return false;
            }
            --nesting;
        } else if (byte == 0U) {
            return false;
        }
    }
    return !in_string && !escaped && nesting == 0U;
}

static bool contains_escaped_nul(const uint8_t* bytes, uint32_t length) {
    static const uint8_t escaped_nul[] = {'\\', 'u', '0', '0', '0', '0'};
    if (length < sizeof(escaped_nul)) {
        return false;
    }
    for (uint32_t offset = 0U; offset <= length - sizeof(escaped_nul); ++offset) {
        if (memcmp(bytes + offset, escaped_nul, sizeof(escaped_nul)) == 0) {
            return true;
        }
    }
    return false;
}

static bool unique_object_keys(const cJSON* object) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (const cJSON* item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL) {
            return false;
        }
        for (const cJSON* previous = object->child; previous != item; previous = previous->next) {
            if (strcmp(previous->string, item->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

static const cJSON* select_display_name(const cJSON* values, const char* default_locale, const char* effective_locale) {
    if (!valid_canonical_locale(effective_locale)) {
        effective_locale = default_locale;
    }
    const cJSON* selected = cJSON_GetObjectItemCaseSensitive(values, effective_locale);
    if (selected == NULL) {
        for (const cJSON* item = values->child; item != NULL; item = item->next) {
            if (same_language_and_script(effective_locale, item->string)) {
                selected = item;
                break;
            }
        }
    }
    if (selected == NULL) {
        for (const cJSON* item = values->child; item != NULL; item = item->next) {
            if (bare_language_match(effective_locale, item->string)) {
                selected = item;
                break;
            }
        }
    }
    return selected != NULL ? selected : cJSON_GetObjectItemCaseSensitive(values, default_locale);
}

static bool valid_component_identifier(const char* value) {
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH);
    if (length == 0U || length >= MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH || value[0] < 'a' || value[0] > 'z') {
        return false;
    }
    bool separator = false;
    for (size_t index = 0U; index < length; ++index) {
        const char byte = value[index];
        const bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
        const bool current_separator = byte == '.' || byte == '_' || byte == '-';
        if ((!alphanumeric && !current_separator) || (current_separator && (separator || index + 1U == length))) {
            return false;
        }
        separator = current_separator;
    }
    return true;
}

static bool valid_semver(const char* value) {
    if (value == NULL) {
        return false;
    }
    const size_t length = strnlen(value, MICROPIXEL_BUNDLE_PACKAGE_VERSION_MAX_LENGTH + 1U);
    if (length == 0U || length > MICROPIXEL_BUNDLE_PACKAGE_VERSION_MAX_LENGTH) {
        return false;
    }
    uint32_t components = 0U;
    for (size_t offset = 0U; offset < length;) {
        const size_t start = offset;
        while (offset < length && value[offset] >= '0' && value[offset] <= '9') {
            ++offset;
        }
        if (offset == start || (offset - start > 1U && value[start] == '0') || ++components > 3U) {
            return false;
        }
        if (offset == length) {
            break;
        }
        if (value[offset++] != '.') {
            return false;
        }
    }
    return components == 3U;
}

static bool parse_display_name(const cJSON* root, const char* effective_locale, uint8_t* display_name_out) {
    const cJSON* display_name = cJSON_GetObjectItemCaseSensitive(root, "display_name");
    const cJSON* default_item =
        cJSON_IsObject(display_name) ? cJSON_GetObjectItemCaseSensitive(display_name, "default") : NULL;
    const cJSON* values =
        cJSON_IsObject(display_name) ? cJSON_GetObjectItemCaseSensitive(display_name, "values") : NULL;
    bool valid = cJSON_IsObject(display_name) && unique_object_keys(display_name) && cJSON_IsString(default_item) &&
                 valid_canonical_locale(default_item->valuestring) && unique_object_keys(values);
    uint32_t value_count = 0U;
    if (valid) {
        for (const cJSON* item = values->child; item != NULL; item = item->next) {
            const size_t name_length = cJSON_IsString(item) ? strlen(item->valuestring) : 0U;
            if (++value_count > MICROPIXEL_BUNDLE_METADATA_MAX_LOCALES || !valid_canonical_locale(item->string) ||
                name_length == 0U || name_length > MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH ||
                !valid_display_name((const uint8_t*)item->valuestring, (uint32_t)name_length)) {
                valid = false;
                break;
            }
        }
        valid =
            valid && value_count != 0U && cJSON_GetObjectItemCaseSensitive(values, default_item->valuestring) != NULL;
    }
    const cJSON* selected = valid ? select_display_name(values, default_item->valuestring, effective_locale) : NULL;
    if (!cJSON_IsString(selected)) {
        return false;
    }
    if (display_name_out != NULL) {
        const size_t selected_length = strlen(selected->valuestring);
        memcpy(display_name_out, selected->valuestring, selected_length + 1U);
    }
    return true;
}

static bool parse_font_role(const cJSON* fonts, const char* role, uint32_t* asset_id_out) {
    const cJSON* record = cJSON_GetObjectItemCaseSensitive(fonts, role);
    if (!cJSON_IsObject(record) || !unique_object_keys(record)) {
        return false;
    }
    const cJSON* asset = cJSON_GetObjectItemCaseSensitive(record, "asset");
    const cJSON* style = cJSON_GetObjectItemCaseSensitive(record, "style");
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(record, "size");
    const cJSON* bpp = cJSON_GetObjectItemCaseSensitive(record, "bpp");
    if (!cJSON_IsString(asset) || !valid_component_identifier(asset->valuestring) || !cJSON_IsString(style) ||
        strcmp(style->valuestring, "regular") != 0 || !cJSON_IsNumber(size) || size->valuedouble < 1.0 ||
        size->valuedouble > 4096.0 || size->valuedouble != (double)size->valueint || !cJSON_IsNumber(bpp) ||
        (bpp->valueint != 1 && bpp->valueint != 2 && bpp->valueint != 4 && bpp->valueint != 8) ||
        bpp->valuedouble != (double)bpp->valueint) {
        return false;
    }
    *asset_id_out = fnv1a32((const uint8_t*)asset->valuestring, strlen(asset->valuestring));
    return *asset_id_out != 0U;
}

static bool parse_component_metadata(const cJSON* root, micropixel_bundle_metadata_t* metadata_out) {
    const cJSON* package_type = cJSON_GetObjectItemCaseSensitive(root, "package_type");
    const cJSON* component_type = cJSON_GetObjectItemCaseSensitive(root, "component_type");
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON* languages = cJSON_GetObjectItemCaseSensitive(root, "languages");
    const cJSON* font_bundle = cJSON_GetObjectItemCaseSensitive(root, "font_bundle");
    const cJSON* charset = cJSON_GetObjectItemCaseSensitive(root, "charset");
    const cJSON* fonts = cJSON_GetObjectItemCaseSensitive(root, "fonts");
    if (!cJSON_IsString(package_type) || strcmp(package_type->valuestring, "component") != 0 ||
        !cJSON_IsString(component_type) || strcmp(component_type->valuestring, "font") != 0 ||
        !cJSON_IsString(version) || !valid_semver(version->valuestring) || !cJSON_IsArray(languages) ||
        !cJSON_IsString(font_bundle) || !valid_component_identifier(font_bundle->valuestring) ||
        !cJSON_IsString(charset) || !valid_component_identifier(charset->valuestring) || !cJSON_IsObject(fonts) ||
        !unique_object_keys(fonts) || cJSON_GetArraySize(languages) <= 0 ||
        cJSON_GetArraySize(languages) > (int)MICROPIXEL_BUNDLE_METADATA_MAX_LOCALES) {
        return false;
    }
    const char* roles[MICROPIXEL_BUNDLE_FONT_ROLE_COUNT] = {"small", "medium", "large", "title"};
    if (cJSON_GetArraySize(fonts) != (int)MICROPIXEL_BUNDLE_FONT_ROLE_COUNT) {
        return false;
    }
    for (uint32_t index = 0U; index < MICROPIXEL_BUNDLE_FONT_ROLE_COUNT; ++index) {
        if (!parse_font_role(fonts, roles[index], &metadata_out->font_asset_ids[index])) {
            return false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (metadata_out->font_asset_ids[previous] == metadata_out->font_asset_ids[index]) {
                return false;
            }
        }
    }
    uint32_t language_count = 0U;
    for (const cJSON* item = languages->child; item != NULL; item = item->next) {
        if (!cJSON_IsString(item) || !valid_canonical_locale(item->valuestring)) {
            return false;
        }
        for (uint32_t previous = 0U; previous < language_count; ++previous) {
            if (strcmp((const char*)metadata_out->languages[previous], item->valuestring) == 0) {
                return false;
            }
        }
        memcpy(metadata_out->languages[language_count], item->valuestring, strlen(item->valuestring) + 1U);
        ++language_count;
    }
    metadata_out->package_type = MICROPIXEL_BUNDLE_PACKAGE_COMPONENT;
    metadata_out->component_type = MICROPIXEL_BUNDLE_COMPONENT_FONT;
    metadata_out->language_count = language_count;
    memcpy(metadata_out->package_version, version->valuestring, strlen(version->valuestring) + 1U);
    return true;
}

static bool parse_package_metadata_json(const uint8_t* bytes, uint32_t length, const char* effective_locale,
                                        micropixel_bundle_metadata_t* metadata_out) {
    if (!safe_json_envelope(bytes, length) || contains_escaped_nul(bytes, length)) {
        return false;
    }
    const char* parse_end = NULL;
    cJSON* root = cJSON_ParseWithLengthOpts((const char*)bytes, length + 1U, &parse_end, true);
    if (root == NULL || !unique_object_keys(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON* provisional_version = cJSON_GetObjectItemCaseSensitive(root, "metadata_version");
    const bool provisional_localized = cJSON_IsNumber(provisional_version) && provisional_version->valueint == 2 &&
                                       provisional_version->valuedouble == 2.0;
    bool valid = ((cJSON_IsNumber(schema) && schema->valueint == MICROPIXEL_BUNDLE_METADATA_SCHEMA_VERSION &&
                   schema->valuedouble == (double)schema->valueint) ||
                  provisional_localized) &&
                 parse_display_name(root, effective_locale, metadata_out->display_name);
    if (valid && provisional_localized) {
        metadata_out->package_type = MICROPIXEL_BUNDLE_PACKAGE_APP;
    } else if (valid) {
        const cJSON* package_type = cJSON_GetObjectItemCaseSensitive(root, "package_type");
        if (cJSON_IsString(package_type) && strcmp(package_type->valuestring, "app") == 0) {
            metadata_out->package_type = MICROPIXEL_BUNDLE_PACKAGE_APP;
        } else {
            valid = parse_component_metadata(root, metadata_out);
        }
    }
    if (valid) {
        metadata_out->metadata_schema_version = MICROPIXEL_BUNDLE_METADATA_SCHEMA_VERSION;
    }
    cJSON_Delete(root);
    return valid;
}

static bool valid_launch_asset_section(const micropixel_bundle_section_t* section) {
    if (section == NULL || section->size == 0U || section->width == 0U || section->height == 0U) {
        return false;
    }
    return (section->format == MICROPIXEL_BUNDLE_FORMAT_JPEG || section->format == MICROPIXEL_BUNDLE_FORMAT_PNG) &&
           section->stride == 0U;
}

static bool read_logical(const bundlefs_file_t* file, uint32_t logical_offset, void* destination, uint32_t size) {
    return bundlefs_read(file, logical_offset, destination, size) == BUNDLEFS_OK;
}

static bool read_package_metadata(const bundlefs_file_t* file, const micropixel_bundle_header_t* header,
                                  const char* effective_locale, micropixel_bundle_metadata_t* metadata_out) {
    const uint32_t toc_end = header->toc_offset + header->section_count * sizeof(micropixel_bundle_section_t);
    bool metadata_found = false;
    micropixel_bundle_metadata_t metadata = {0};
    for (uint32_t index = 0U; index < header->section_count; ++index) {
        micropixel_bundle_section_t section;
        const uint32_t section_offset = header->toc_offset + index * sizeof(section);
        if (!read_logical(file, section_offset, &section, sizeof(section))) {
            return false;
        }
        if (section.kind != MICROPIXEL_BUNDLE_SECTION_APP_METADATA) {
            continue;
        }
        const bool legacy = section.format == MICROPIXEL_BUNDLE_FORMAT_UTF8;
        const uint32_t maximum_size =
            legacy ? MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH : MICROPIXEL_BUNDLE_METADATA_MAX_LENGTH;
        if (metadata_found || section.id != 0U || section.size == 0U || section.size > maximum_size ||
            section.offset < toc_end || (section.offset & 63U) != 0U || section.offset > header->bundle_size ||
            section.size > header->bundle_size - section.offset ||
            (!legacy && section.format != MICROPIXEL_BUNDLE_FORMAT_PACKAGE_METADATA_JSON) || section.width != 0U ||
            section.height != 0U || section.stride != 0U || section.flags != 0U || section.reserved0 != 0U ||
            section.reserved1 != 0U) {
            return false;
        }
        uint8_t* payload = malloc(section.size + 1U);
        if (payload == NULL || !read_logical(file, section.offset, payload, section.size) ||
            fnv1a32(payload, section.size) != section.hash) {
            free(payload);
            return false;
        }
        payload[section.size] = 0U;
        const bool valid = legacy ? valid_display_name(payload, section.size)
                                  : parse_package_metadata_json(payload, section.size, effective_locale, &metadata);
        if (legacy && valid) {
            memcpy(metadata.display_name, payload, section.size);
            metadata.metadata_schema_version = 0U;
            metadata.package_type = MICROPIXEL_BUNDLE_PACKAGE_APP;
        }
        free(payload);
        if (!valid) {
            return false;
        }
        metadata_found = true;
    }
    if (!metadata_found) {
        return false;
    }
    if (metadata_out != NULL) {
        *metadata_out = metadata;
    }
    return true;
}

static bool read_valid_header(const bundlefs_file_t* file, micropixel_bundle_header_t* header_out,
                              const char* effective_locale, micropixel_bundle_metadata_t* metadata_out) {
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
        !read_package_metadata(file, &header, effective_locale, metadata_out)) {
        return false;
    }
    *header_out = header;
    return true;
}

bool micropixel_read_bundle_metadata(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out) {
    return micropixel_read_bundle_metadata_for_locale(file, "en", metadata_out);
}

bool micropixel_read_bundle_metadata_for_locale(const bundlefs_file_t* file, const char* effective_locale,
                                                micropixel_bundle_metadata_t* metadata_out) {
    if (metadata_out == NULL) {
        return false;
    }
    memset(metadata_out, 0, sizeof(*metadata_out));
    micropixel_bundle_header_t header;
    if (!read_valid_header(file, &header, effective_locale, metadata_out)) {
        return false;
    }
    metadata_out->bundle_size = header.bundle_size;
    memcpy(metadata_out->app_id, header.app_id, header.app_id_length);
    return true;
}

static uint16_t read_u16_le(const uint8_t* data) { return (uint16_t)data[0] | ((uint16_t)data[1] << 8U); }

static uint32_t read_u32_le(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) | ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static bool valid_font_cbin_envelope(const uint8_t* data, uint32_t size) {
    if (data == NULL || size < MICROPIXEL_FONT_CBIN_HEADER_SIZE ||
        memcmp(data, MICROPIXEL_FONT_CBIN_MAGIC, MICROPIXEL_FONT_CBIN_MAGIC_SIZE) != 0 ||
        read_u16_le(data + MICROPIXEL_FONT_CBIN_OFFSET_HEADER_VERSION) != MICROPIXEL_FONT_CBIN_HEADER_VERSION ||
        read_u16_le(data + MICROPIXEL_FONT_CBIN_OFFSET_HEADER_SIZE) != MICROPIXEL_FONT_CBIN_HEADER_SIZE ||
        read_u32_le(data + MICROPIXEL_FONT_CBIN_OFFSET_TOTAL_SIZE) != size ||
        read_u32_le(data + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_OFFSET) != MICROPIXEL_FONT_CBIN_HEADER_SIZE ||
        read_u32_le(data + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SIZE) != size - MICROPIXEL_FONT_CBIN_HEADER_SIZE ||
        read_u32_le(data + MICROPIXEL_FONT_CBIN_OFFSET_FORMAT) != MICROPIXEL_FONT_CBIN_FORMAT_LVGL_V1 ||
        data[MICROPIXEL_FONT_CBIN_OFFSET_ENDIAN] != MICROPIXEL_FONT_CBIN_ENDIAN_LITTLE ||
        data[MICROPIXEL_FONT_CBIN_OFFSET_POINTER_SIZE] != MICROPIXEL_FONT_CBIN_POINTER_SIZE ||
        data[MICROPIXEL_FONT_CBIN_OFFSET_GLYPH_DSC_LAYOUT] != MICROPIXEL_FONT_CBIN_GLYPH_DSC_LARGE ||
        data[MICROPIXEL_FONT_CBIN_OFFSET_FLAGS] != 0U ||
        read_u16_le(data + MICROPIXEL_FONT_CBIN_OFFSET_FONT_SIZE_PX) == 0U) {
        return false;
    }
    uint8_t digest[MICROPIXEL_FONT_CBIN_SHA256_SIZE];
    size_t digest_size = 0U;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, data + MICROPIXEL_FONT_CBIN_HEADER_SIZE,
                            size - MICROPIXEL_FONT_CBIN_HEADER_SIZE, digest, sizeof(digest),
                            &digest_size) == PSA_SUCCESS &&
           digest_size == sizeof(digest) &&
           memcmp(digest, data + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SHA256, sizeof(digest)) == 0;
}

bool micropixel_validate_component_package(const bundlefs_file_t* file, micropixel_bundle_metadata_t* metadata_out) {
    micropixel_bundle_header_t header;
    micropixel_bundle_metadata_t metadata = {0};
    if (metadata_out == NULL || !read_valid_header(file, &header, "en", &metadata) ||
        metadata.package_type != MICROPIXEL_BUNDLE_PACKAGE_COMPONENT ||
        metadata.component_type != MICROPIXEL_BUNDLE_COMPONENT_FONT || header.launch_asset_id != 0U) {
        return false;
    }
    bundlefs_mapping_t mapping;
    if (bundlefs_mmap(file, 0U, header.bundle_size, &mapping) != BUNDLEFS_OK) {
        return false;
    }
    const uint8_t* bundle = mapping.data;
    const micropixel_bundle_header_t* mapped_header = (const micropixel_bundle_header_t*)bundle;
    const micropixel_bundle_section_t* sections =
        (const micropixel_bundle_section_t*)(bundle + mapped_header->toc_offset);
    bool valid = memcmp(mapped_header, &header, sizeof(header)) == 0;
    bool metadata_found = false;
    bool font_found[MICROPIXEL_BUNDLE_FONT_ROLE_COUNT] = {false};
    const uint32_t toc_end = mapped_header->toc_offset + mapped_header->section_count * sizeof(*sections);
    for (uint32_t index = 0U; valid && index < mapped_header->section_count; ++index) {
        const micropixel_bundle_section_t* section = &sections[index];
        valid = section->size != 0U && section->offset >= toc_end && (section->offset & 63U) == 0U &&
                section->offset <= mapped_header->bundle_size &&
                section->size <= mapped_header->bundle_size - section->offset && section->flags == 0U &&
                section->reserved0 == 0U && section->reserved1 == 0U;
        for (uint32_t previous = 0U; valid && previous < index; ++previous) {
            const micropixel_bundle_section_t* other = &sections[previous];
            const uint64_t section_end = (uint64_t)section->offset + section->size;
            const uint64_t other_end = (uint64_t)other->offset + other->size;
            valid = (uint64_t)section->offset >= other_end || (uint64_t)other->offset >= section_end;
        }
        const uint8_t* section_data = bundle + section->offset;
        valid = valid && fnv1a32(section_data, section->size) == section->hash;
        if (!valid) {
            break;
        }
        if (section->kind == MICROPIXEL_BUNDLE_SECTION_APP_METADATA) {
            valid = !metadata_found && section->id == 0U &&
                    section->format == MICROPIXEL_BUNDLE_FORMAT_PACKAGE_METADATA_JSON && section->width == 0U &&
                    section->height == 0U && section->stride == 0U;
            metadata_found = valid;
            continue;
        }
        if (section->kind != MICROPIXEL_BUNDLE_SECTION_FONT || section->id == 0U ||
            section->format != MICROPIXEL_BUNDLE_FORMAT_LVGL_CBIN_V1 || section->width != 0U || section->height != 0U ||
            section->stride != 0U || !valid_font_cbin_envelope(section_data, section->size)) {
            valid = false;
            break;
        }
        bool matched = false;
        for (uint32_t role = 0U; role < MICROPIXEL_BUNDLE_FONT_ROLE_COUNT; ++role) {
            if (metadata.font_asset_ids[role] == section->id) {
                valid = !font_found[role];
                font_found[role] = valid;
                matched = true;
                break;
            }
        }
        valid = valid && matched;
    }
    valid = valid && metadata_found;
    for (uint32_t role = 0U; valid && role < MICROPIXEL_BUNDLE_FONT_ROLE_COUNT; ++role) {
        valid = font_found[role];
    }
    bundlefs_munmap(&mapping);
    if (!valid) {
        return false;
    }
    metadata.bundle_size = header.bundle_size;
    memcpy(metadata.app_id, header.app_id, header.app_id_length);
    *metadata_out = metadata;
    return true;
}

bool micropixel_open_launch_asset(const bundlefs_file_t* file, micropixel_bundle_asset_mapping_t* mapping_out) {
    if (mapping_out == NULL) {
        return false;
    }
    memset(mapping_out, 0, sizeof(*mapping_out));
    micropixel_bundle_header_t header;
    if (!read_valid_header(file, &header, NULL, NULL) || header.launch_asset_id == 0U) {
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
    micropixel_bundle_metadata_t metadata;
    if (!read_valid_header(file, &header, NULL, &metadata) || metadata.package_type != MICROPIXEL_BUNDLE_PACKAGE_APP) {
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
            const bool legacy = section->format == MICROPIXEL_BUNDLE_FORMAT_UTF8;
            if (metadata_found || section->id != 0U ||
                section->size >
                    (legacy ? MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH : MICROPIXEL_BUNDLE_METADATA_MAX_LENGTH) ||
                (!legacy && section->format != MICROPIXEL_BUNDLE_FORMAT_PACKAGE_METADATA_JSON) ||
                section->width != 0U || section->height != 0U || section->stride != 0U ||
                (legacy && !valid_display_name(section_data, section->size))) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
            metadata_found = true;
        } else if (section->kind == MICROPIXEL_BUNDLE_SECTION_FONT) {
            if (section->id == 0U || section->format != MICROPIXEL_BUNDLE_FORMAT_LVGL_CBIN_V1 || section->width != 0U ||
                section->height != 0U || section->stride != 0U) {
                bundlefs_munmap(&bundlefs_mapping);
                return false;
            }
            for (uint32_t previous = 0U; previous < index; ++previous) {
                if (sections[previous].kind == MICROPIXEL_BUNDLE_SECTION_FONT && sections[previous].id == section->id) {
                    bundlefs_munmap(&bundlefs_mapping);
                    return false;
                }
            }
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

bool micropixel_bundle_find_font(const micropixel_aot_package_t* package, uint32_t resource_id,
                                 micropixel_bundle_font_view_t* view_out) {
    if (package == NULL || view_out == NULL || package->bundle_mapping == NULL || resource_id == 0U) {
        return false;
    }
    memset(view_out, 0, sizeof(*view_out));
    for (uint32_t index = 0U; index < package->section_count; ++index) {
        const micropixel_bundle_section_t* section = &package->sections[index];
        if (section->kind == MICROPIXEL_BUNDLE_SECTION_FONT && section->id == resource_id) {
            view_out->data = package->bundle_mapping + section->offset;
            view_out->size = section->size;
            view_out->content_hash = section->hash;
            return true;
        }
    }
    return false;
}
