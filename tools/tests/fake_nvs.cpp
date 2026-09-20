// SPDX-License-Identifier: Apache-2.0
#include "fake_nvs.hpp"

#include <cassert>
#include <cstring>

#include "esp_partition.h"

namespace {
struct Handle {
    std::string path;
    int mode;
};
std::map<nvs_handle_t, Handle> handles;
nvs_handle_t next_handle = 1;
esp_err_t Read(nvs_handle_t h, const char* key, nvs_type_t type, void* out, size_t* length) {
    auto& ns = fake_nvs::data.at(handles.at(h).path);
    auto entry = ns.find(key);
    if (entry == ns.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (entry->second.type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out && *length < entry->second.bytes.size()) return ESP_ERR_INVALID_ARG;
    *length = entry->second.bytes.size();
    if (out) std::memcpy(out, entry->second.bytes.data(), *length);
    return ESP_OK;
}
esp_err_t Write(nvs_handle_t h, const char* key, nvs_type_t type, const void* bytes, size_t length) {
    assert(handles.at(h).mode == NVS_READWRITE);
    if (fake_nvs::fail_write) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    auto& value = fake_nvs::data[handles.at(h).path][key];
    value.type = type;
    value.bytes.resize(length);
    if (length) std::memcpy(value.bytes.data(), bytes, length);
    return ESP_OK;
}
}  // namespace
esp_err_t nvs_open_from_partition(const char* partition, const char* name, int mode, nvs_handle_t* out) {
    std::string path = std::string(partition) + "/" + name;
    if (!fake_nvs::data.contains(path) && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    fake_nvs::data[path];
    *out = next_handle++;
    handles[*out] = {path, mode};
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(handles.erase(h) == 1); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char* k, void* out, size_t* len) {
    return Read(h, k, NVS_TYPE_BLOB, out, len);
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* k, const void* in, size_t len) {
    return Write(h, k, NVS_TYPE_BLOB, in, len);
}
esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* out, size_t* len) {
    return Read(h, k, NVS_TYPE_STR, out, len);
}
#define SCALAR(suffix, type, tag)                                          \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char* k, type* out) { \
        size_t len = sizeof(type);                                         \
        return Read(h, k, tag, out, &len);                                 \
    }                                                                      \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char* k, type in) { return Write(h, k, tag, &in, sizeof(in)); }
SCALAR(i32, int32_t, NVS_TYPE_I32)
SCALAR(u32, uint32_t, NVS_TYPE_U32)
SCALAR(u8, uint8_t, NVS_TYPE_U8)
#undef SCALAR
esp_err_t nvs_commit(nvs_handle_t h) {
    return !fake_nvs::fail_commit_partition.empty() &&
                   handles.at(h).path.starts_with(fake_nvs::fail_commit_partition + "/")
               ? ESP_FAIL
               : ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    if (fake_nvs::fail_erase) return ESP_FAIL;
    return fake_nvs::data.at(handles.at(h).path).erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_erase_all(nvs_handle_t h) {
    if (fake_nvs::fail_erase) return ESP_FAIL;
    fake_nvs::data.at(handles.at(h).path).clear();
    return ESP_OK;
}
esp_err_t nvs_find_key(nvs_handle_t h, const char* key, nvs_type_t* type) {
    auto& ns = fake_nvs::data.at(handles.at(h).path);
    auto entry = ns.find(key);
    if (entry == ns.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (type) *type = entry->second.type;
    return ESP_OK;
}
struct TestNvsIterator {
    fake_nvs::Namespace* ns;
    fake_nvs::Namespace::iterator entry;
};
esp_err_t nvs_entry_find_in_handle(nvs_handle_t h, nvs_type_t, nvs_iterator_t* out) {
    auto& ns = fake_nvs::data.at(handles.at(h).path);
    *out = ns.empty() ? nullptr : new TestNvsIterator{&ns, ns.begin()};
    return *out ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_info(nvs_iterator_t i, nvs_entry_info_t* out) {
    std::strcpy(out->key, i->entry->first.c_str());
    out->type = i->entry->second.type;
    return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* i) {
    if (++(*i)->entry == (*i)->ns->end()) {
        delete *i;
        *i = nullptr;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t i) { delete i; }

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char* label) {
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS);
    assert(std::strcmp(label, "runtime_nvs") == 0);
    static esp_partition_t partition{};
    partition.size = fake_nvs::partition_bytes;
    return fake_nvs::partition_present ? &partition : nullptr;
}

esp_err_t nvs_get_stats(const char* partition_label, nvs_stats_t* stats) {
    assert(std::strcmp(partition_label, "runtime_nvs") == 0);
    *stats = fake_nvs::stats;
    return fake_nvs::stats_status;
}
