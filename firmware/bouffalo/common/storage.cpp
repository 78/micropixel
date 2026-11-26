// SPDX-License-Identifier: Apache-2.0
#include "storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "micropixel_mutex.hpp"
#include "nvs.h"
#include "runtime/services/app_storage.hpp"
extern "C" {
#include "lfs.h"
}
namespace {
constexpr uint32_t kEraseSize = 4096;
BouffaloMutex flash_lock, kv_lock;
#ifdef LFS_THREADSAFE
BouffaloMutex fs_lock;
#endif
#ifdef LFS_THREADSAFE
int FsLock(const lfs_config*) {
    fs_lock.lock();
    return 0;
}
int FsUnlock(const lfs_config*) {
    fs_lock.unlock();
    return 0;
}
#endif
bflb_mtd_handle_t kv_mtd{};
lfs_t kv{};
lfs_config kv_cfg{};
uint8_t read_cache[256], prog_cache[256], lookahead[32];
bool kv_ready{};
struct Handle {
    char name[16];
    bool writable;
};
bool Name(const char* s) {
    if (!s || !*s || strlen(s) > 15 || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' ||
              *s == '-' || *s == '.'))
            return false;
    return true;
}
int Read(const lfs_config* c, lfs_block_t b, lfs_off_t o, void* d, lfs_size_t n) {
    std::lock_guard g(flash_lock);
    return bflb_mtd_read(kv_mtd, b * c->block_size + o, n, (uint8_t*)d) == 0 ? 0 : LFS_ERR_IO;
}
int Prog(const lfs_config* c, lfs_block_t b, lfs_off_t o, const void* d, lfs_size_t n) {
    std::lock_guard g(flash_lock);
    return bflb_mtd_write(kv_mtd, b * c->block_size + o, n, (const uint8_t*)d) == 0 ? 0 : LFS_ERR_IO;
}
int Erase(const lfs_config* c, lfs_block_t b) {
    std::lock_guard g(flash_lock);
    return bflb_mtd_erase(kv_mtd, b * c->block_size, c->block_size) == 0 ? 0 : LFS_ERR_IO;
}
int Sync(const lfs_config*) { return 0; }
void Path(Handle* h, const char* k, char (&p)[40]) { snprintf(p, sizeof(p), "/%s/%s", h->name, k); }
esp_err_t Convert(int e) {
    return e >= 0               ? ESP_OK
           : e == LFS_ERR_NOENT ? ESP_ERR_NVS_NOT_FOUND
           : e == LFS_ERR_NOSPC ? ESP_ERR_NVS_NOT_ENOUGH_SPACE
           : e == LFS_ERR_NOMEM ? ESP_ERR_NO_MEM
                                : ESP_FAIL;
}
esp_err_t Get(nvs_handle_t id, const char* k, nvs_type_t type, void* out, size_t* len, nvs_type_t* found = nullptr) {
    if (!id || !Name(k) || !len) return ESP_ERR_INVALID_ARG;
    std::lock_guard g(kv_lock);
    char p[40];
    Path((Handle*)id, k, p);
    lfs_file_t f{};
    int e = lfs_file_open(&kv, &f, p, LFS_O_RDONLY);
    if (e < 0) return Convert(e);
    uint8_t t{};
    e = lfs_file_read(&kv, &f, &t, 1);
    lfs_soff_t size = lfs_file_size(&kv, &f);
    if (e != 1 || size < 1) {
        lfs_file_close(&kv, &f);
        return ESP_FAIL;
    }
    if (found) *found = (nvs_type_t)t;
    if (type != NVS_TYPE_ANY && t != type) {
        lfs_file_close(&kv, &f);
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    size_t required = (size_t)size - 1;
    esp_err_t status = ESP_OK;
    if (out) {
        if (*len < required)
            status = ESP_ERR_NVS_INVALID_LENGTH;
        else if (lfs_file_read(&kv, &f, out, required) != (int)required)
            status = ESP_FAIL;
    }
    *len = required;
    if (lfs_file_close(&kv, &f) < 0) status = ESP_FAIL;
    return status;
}
esp_err_t Set(nvs_handle_t id, const char* k, nvs_type_t type, const void* d, size_t n) {
    if (!id || !Name(k) || (!d && n) || n > 4096) return ESP_ERR_INVALID_ARG;
    auto* h = (Handle*)id;
    if (!h->writable) return ESP_ERR_INVALID_STATE;
    std::lock_guard g(kv_lock);
    char p[40], tmp[40];
    Path(h, k, p);
    Path(h, "@pending", tmp);
    lfs_file_t f{};
    int e = lfs_file_open(&kv, &f, tmp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (e < 0) return Convert(e);
    uint8_t t = (uint8_t)type;
    if (lfs_file_write(&kv, &f, &t, 1) != 1 || lfs_file_write(&kv, &f, d, n) != (int)n) e = LFS_ERR_IO;
    if (e >= 0) e = lfs_file_sync(&kv, &f);
    int close = lfs_file_close(&kv, &f);
    if (e >= 0) e = close;
    if (e >= 0) e = lfs_rename(&kv, tmp, p);
    if (e < 0) lfs_remove(&kv, tmp);
    return Convert(e);
}
}  // namespace
struct mpx_nvs_iterator {
    nvs_entry_info_t entries[64];
    size_t count{}, index{};
};
namespace mpx_port {
FlashStorage::~FlashStorage() {
    if (handle_) (void)bflb_mtd_close(handle_);
}
bool FlashStorage::Initialize() {
    if (handle_) return true;
    bflb_mtd_handle_t handle{};
    if (bflb_mtd_open("appstore", &handle, 0)) return false;
    bflb_mtd_info_t info{};
    if (bflb_mtd_info(handle, &info) || info.size == 0 || info.size % kEraseSize != 0) {
        (void)bflb_mtd_close(handle);
        return false;
    }
    handle_ = handle;
    geometry_ = {info.size, kEraseSize, 1, false, 0};
    return true;
}
std::expected<void, micropixel::device::BlockStorageError> FlashStorage::Read(uint64_t o, std::span<uint8_t> d) {
    using E = micropixel::device::BlockStorageError;
    if (!handle_) return std::unexpected(E::kUnavailable);
    if (o > geometry_.size_bytes || d.size() > geometry_.size_bytes - o) return std::unexpected(E::kInvalidArgument);
    if (d.empty()) return {};
    std::lock_guard g(flash_lock);
    if (bflb_mtd_read(handle_, o, d.size(), d.data())) return std::unexpected(E::kIo);
    return {};
}
std::expected<void, micropixel::device::BlockStorageError> FlashStorage::Program(uint64_t o,
                                                                                 std::span<const uint8_t> d) {
    using E = micropixel::device::BlockStorageError;
    if (!handle_) return std::unexpected(E::kUnavailable);
    if (o > geometry_.size_bytes || d.size() > geometry_.size_bytes - o) return std::unexpected(E::kInvalidArgument);
    if (d.empty()) return {};
    std::lock_guard g(flash_lock);
    if (bflb_mtd_write(handle_, o, d.size(), d.data())) return std::unexpected(E::kIo);
    return {};
}
std::expected<void, micropixel::device::BlockStorageError> FlashStorage::Erase(uint64_t o, uint64_t n) {
    using E = micropixel::device::BlockStorageError;
    if (!handle_) return std::unexpected(E::kUnavailable);
    if (o % kEraseSize || n % kEraseSize || o > geometry_.size_bytes || n > geometry_.size_bytes - o)
        return std::unexpected(E::kInvalidArgument);
    if (n == 0) return {};
    std::lock_guard g(flash_lock);
    if (bflb_mtd_erase(handle_, o, n)) return std::unexpected(E::kIo);
    return {};
}
bool InitializeKv() {
    std::lock_guard g(kv_lock);
    if (kv_ready) return true;
    bflb_mtd_info_t info{};
    if (bflb_mtd_open("media", &kv_mtd, 0)) return false;
    const auto close_on_failure = [] {
        (void)bflb_mtd_close(kv_mtd);
        kv_mtd = nullptr;
        return false;
    };
    if (bflb_mtd_info(kv_mtd, &info) || info.size < 2 * kEraseSize || info.size % kEraseSize != 0)
        return close_on_failure();
#ifdef LFS_THREADSAFE
    kv_cfg.lock = FsLock;
    kv_cfg.unlock = FsUnlock;
#endif
    kv_cfg.read = Read;
    kv_cfg.prog = Prog;
    kv_cfg.erase = Erase;
    kv_cfg.sync = Sync;
    kv_cfg.read_size = 16;
    kv_cfg.prog_size = 16;
    kv_cfg.block_size = kEraseSize;
    kv_cfg.block_count = info.size / kEraseSize;
    kv_cfg.block_cycles = 128;
    kv_cfg.cache_size = 256;
    kv_cfg.lookahead_size = sizeof(lookahead);
    kv_cfg.read_buffer = read_cache;
    kv_cfg.prog_buffer = prog_cache;
    kv_cfg.lookahead_buffer = lookahead;
    int e = lfs_mount(&kv, &kv_cfg);
    if (e != 0 && e != LFS_ERR_CORRUPT) return close_on_failure();
    if (e == LFS_ERR_CORRUPT) {  // Only initialize a completely erased partition; preserve corrupt or foreign data.
        uint8_t page[256];
        bool blank = true;
        for (uint32_t o = 0; o < info.size && blank; o += sizeof(page)) {
            std::lock_guard flash_guard(flash_lock);
            if (bflb_mtd_read(kv_mtd, o, sizeof(page), page)) return close_on_failure();
            for (auto b : page)
                if (b != 255) {
                    blank = false;
                    break;
                }
        }
        if (!blank || lfs_format(&kv, &kv_cfg) < 0 || lfs_mount(&kv, &kv_cfg) < 0) return close_on_failure();
    }
    kv_ready = true;
    return true;
}
}  // namespace mpx_port
extern "C" {
esp_err_t nvs_open_from_partition(const char* p, const char* n, nvs_open_mode_t mode, nvs_handle_t* out) {
    if (!p || strcmp(p, "runtime_nvs") || !Name(n) || !out) return ESP_ERR_INVALID_ARG;
    if (!kv_ready) return ESP_ERR_INVALID_STATE;
    std::lock_guard g(kv_lock);
    char path[20];
    snprintf(path, sizeof(path), "/%s", n);
    lfs_info i{};
    int e = lfs_stat(&kv, path, &i);
    if (e == LFS_ERR_NOENT && mode == NVS_READWRITE) e = lfs_mkdir(&kv, path);
    if (e < 0) return Convert(e);
    auto* h = (Handle*)calloc(1, sizeof(Handle));
    if (!h) return ESP_ERR_NO_MEM;
    strcpy(h->name, n);
    h->writable = mode == NVS_READWRITE;
    *out = (uintptr_t)h;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { free((void*)h); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char* k, uint32_t* d) {
    size_t n = 4;
    if (!d) return ESP_ERR_INVALID_ARG;
    const auto result = Get(h, k, NVS_TYPE_U32, d, &n);
    return result == ESP_OK && n != sizeof(*d) ? ESP_FAIL : result;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* d) {
    size_t n = 1;
    if (!d) return ESP_ERR_INVALID_ARG;
    const auto result = Get(h, k, NVS_TYPE_U8, d, &n);
    return result == ESP_OK && n != sizeof(*d) ? ESP_FAIL : result;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* k, void* d, size_t* n) { return Get(h, k, NVS_TYPE_BLOB, d, n); }
esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* d, size_t* n) { return Get(h, k, NVS_TYPE_STR, d, n); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char* k, uint32_t d) { return Set(h, k, NVS_TYPE_U32, &d, 4); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t d) { return Set(h, k, NVS_TYPE_U8, &d, 1); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char* k, const void* d, size_t n) {
    return Set(h, k, NVS_TYPE_BLOB, d, n);
}
esp_err_t nvs_find_key(nvs_handle_t h, const char* k, nvs_type_t* t) {
    size_t n{};
    return Get(h, k, NVS_TYPE_ANY, nullptr, &n, t);
}
esp_err_t nvs_commit(nvs_handle_t h) {
    return h ? ESP_OK : ESP_ERR_INVALID_ARG;
}  // Mutations already sync and atomically rename.
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) {
    if (!h || !Name(k)) return ESP_ERR_INVALID_ARG;
    if (!((Handle*)h)->writable) return ESP_ERR_INVALID_STATE;
    std::lock_guard g(kv_lock);
    char p[40];
    Path((Handle*)h, k, p);
    return Convert(lfs_remove(&kv, p));
}
esp_err_t nvs_entry_find_in_handle(nvs_handle_t h, nvs_type_t type, nvs_iterator_t* out) {
    if (!h || !out) return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    auto* it = (mpx_nvs_iterator*)calloc(1, sizeof(mpx_nvs_iterator));
    if (!it) return ESP_ERR_NO_MEM;
    {
        std::lock_guard g(kv_lock);
        char p[20];
        snprintf(p, sizeof(p), "/%s", ((Handle*)h)->name);
        lfs_dir_t dir{};
        int e = lfs_dir_open(&kv, &dir, p);
        if (e < 0) {
            free(it);
            return Convert(e);
        }
        lfs_info info{};
        while ((e = lfs_dir_read(&kv, &dir, &info)) > 0) {
            if (info.type != LFS_TYPE_REG || !Name(info.name)) continue;
            if (it->count == 64) {
                e = LFS_ERR_NOSPC;
                break;
            }
            auto& v = it->entries[it->count++];
            strcpy(v.namespace_name, ((Handle*)h)->name);
            strcpy(v.key, info.name);
        }
        lfs_dir_close(&kv, &dir);
        if (e < 0) {
            free(it);
            return Convert(e);
        }
    }
    size_t used = 0;
    for (size_t i = 0; i < it->count; ++i) {
        auto v = it->entries[i];
        esp_err_t e = nvs_find_key(h, v.key, &v.type);
        if (e != ESP_OK) {
            free(it);
            return e;
        }
        if (type == NVS_TYPE_ANY || v.type == type) it->entries[used++] = v;
    }
    it->count = used;
    if (!used) {
        free(it);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = it;
    return ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t i, nvs_entry_info_t* out) {
    if (!i || !out) return ESP_ERR_INVALID_ARG;
    *out = i->entries[i->index];
    return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* i) {
    if (!i || !*i) return ESP_ERR_NVS_NOT_FOUND;
    if (++(*i)->index < (*i)->count) return ESP_OK;
    free(*i);
    *i = nullptr;
    return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_release_iterator(nvs_iterator_t i) { free(i); }
esp_err_t nvs_erase_all(nvs_handle_t h) {
    nvs_iterator_t it{};
    esp_err_t e = nvs_entry_find_in_handle(h, NVS_TYPE_ANY, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t i{};
        nvs_entry_info(it, &i);
        esp_err_t erased = nvs_erase_key(h, i.key);
        if (erased != ESP_OK) {
            nvs_release_iterator(it);
            return erased;
        }
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
}
}
namespace micropixel::runtime {
std::expected<AppStorageUsage, esp_err_t> ReadAppStorageUsage() {
    std::lock_guard g(kv_lock);
    if (!kv_ready) return std::unexpected(ESP_ERR_INVALID_STATE);
    auto used = lfs_fs_size(&kv);
    if (used < 0) return std::unexpected(ESP_FAIL);
    uint32_t total = kv_cfg.block_count * kv_cfg.block_size;
    return AppStorageUsage{total, (uint32_t)used * kv_cfg.block_size, total - (uint32_t)used * kv_cfg.block_size};
}
}  // namespace micropixel::runtime
