#ifndef MICROPIXEL_RUNTIME_SERVICES_STORAGE_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_STORAGE_SERVICE_HPP

#include <cstddef>
#include <cstdint>

#include "nvs.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class StorageService final {
   public:
    explicit StorageService(const micropixel_aot_package_t& package);
    StorageService(const StorageService&) = delete;
    StorageService& operator=(const StorageService&) = delete;
    ~StorageService();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return handle_ != 0U;
    }
    [[nodiscard]] ServiceResult<uint32_t> GetU32(const char* key, uint32_t key_length);
    [[nodiscard]] ServiceResult<void> SetU32(const char* key, uint32_t key_length, uint32_t value);
    [[nodiscard]] ServiceResult<bool> GetBool(const char* key, uint32_t key_length);
    [[nodiscard]] ServiceResult<void> SetBool(const char* key, uint32_t key_length, bool value);
    [[nodiscard]] ServiceResult<uint32_t> GetBytes(const char* key, uint32_t key_length, uint8_t* bytes_out,
                                                   uint32_t capacity);
    [[nodiscard]] ServiceResult<void> SetBytes(const char* key, uint32_t key_length, const uint8_t* bytes,
                                               uint32_t length);
    [[nodiscard]] ServiceResult<void> Remove(const char* key, uint32_t key_length);

   private:
    int32_t NormalizeKey(const char* key, uint32_t key_length, char (&normalized)[NVS_KEY_NAME_MAX_SIZE]) const;
    int32_t PrepareWrite(const char* key, size_t new_size);
    int32_t Commit(esp_err_t mutation_error);
    bool Usage(size_t& bytes_out, size_t& keys_out) const;
    bool ValueSize(const char* key, nvs_type_t type, size_t& size_out) const;
    bool ConsumeWriteToken();

    nvs_handle_t handle_{};
    char namespace_[NVS_NS_NAME_MAX_SIZE]{};
    uint32_t write_tokens_{CONFIG_MICROPIXEL_KV_WRITE_BURST};
    int64_t token_refill_time_us_{};
};

}  // namespace micropixel::runtime

#endif
