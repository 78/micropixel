#pragma once
#include <array>
#include <cstring>
#include <string_view>

#include "cJSON.h"
#include "host/controller/control_types.hpp"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "sdkconfig.h"

#ifndef CONFIG_MICROPIXEL_STORE_PUBLIC_KEY_DER_BASE64
#define CONFIG_MICROPIXEL_STORE_PUBLIC_KEY_DER_BASE64 ""
#define CONFIG_MICROPIXEL_STORE_SIGNING_KID "store-v1"
#endif
#ifndef CONFIG_MICROPIXEL_STORE_PREVIOUS_PUBLIC_KEY_DER_BASE64
#define CONFIG_MICROPIXEL_STORE_PREVIOUS_PUBLIC_KEY_DER_BASE64 ""
#define CONFIG_MICROPIXEL_STORE_PREVIOUS_SIGNING_KID ""
#endif

namespace micropixel::firmware::remote_control {
inline bool StoreTrustConfigured() { return CONFIG_MICROPIXEL_STORE_PUBLIC_KEY_DER_BASE64[0] != '\0'; }
inline bool DecodeStoreBase64(std::string_view input, uint8_t* output, size_t capacity, size_t& written) {
    std::array<uint8_t, 2048U> text{};
    if (input.empty() || input.size() > text.size() - 4U) return false;
    size_t length = input.size();
    for (size_t i = 0; i < length; ++i) {
        const char c = input[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '+' || c == '/' || c == '='))
            return false;
        text[i] = static_cast<uint8_t>(c == '-' ? '+' : c == '_' ? '/' : c);
    }
    while (length % 4U != 0U) text[length++] = '=';
    return mbedtls_base64_decode(output, capacity, &written, text.data(), length) == 0;
}
inline const char* StoreString(const cJSON* object, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}
inline bool VerifyStoreRelease(const char* envelope, const char* release_id, control::HostCommand& command) {
    if (!StoreTrustConfigured() || envelope == nullptr || std::strlen(envelope) > 1800U) return false;
    const std::string_view text(envelope);
    const size_t first = text.find('.'), second = first == text.npos ? text.npos : text.find('.', first + 1U);
    if (second == text.npos || text.find('.', second + 1U) != text.npos) return false;
    std::array<uint8_t, 256U> header_bytes{};
    std::array<uint8_t, 1024U> payload_bytes{};
    std::array<uint8_t, 64U> signature{};
    size_t header_size = 0, payload_size = 0, signature_size = 0;
    if (!DecodeStoreBase64(text.substr(0, first), header_bytes.data(), header_bytes.size() - 1U, header_size) ||
        !DecodeStoreBase64(text.substr(first + 1U, second - first - 1U), payload_bytes.data(),
                           payload_bytes.size() - 1U, payload_size) ||
        !DecodeStoreBase64(text.substr(second + 1U), signature.data(), signature.size(), signature_size) ||
        signature_size != 64U)
        return false;
    cJSON* header =
        cJSON_ParseWithLengthOpts(reinterpret_cast<char*>(header_bytes.data()), header_size + 1U, nullptr, true);
    const char* key = nullptr;
    if (header != nullptr && std::strcmp(StoreString(header, "alg"), "ES256") == 0 &&
        std::strcmp(StoreString(header, "typ"), "MPX-RELEASE") == 0) {
        if (std::strcmp(StoreString(header, "kid"), CONFIG_MICROPIXEL_STORE_SIGNING_KID) == 0)
            key = CONFIG_MICROPIXEL_STORE_PUBLIC_KEY_DER_BASE64;
        else if (std::strcmp(StoreString(header, "kid"), CONFIG_MICROPIXEL_STORE_PREVIOUS_SIGNING_KID) == 0)
            key = CONFIG_MICROPIXEL_STORE_PREVIOUS_PUBLIC_KEY_DER_BASE64;
    }
    cJSON_Delete(header);
    if (key == nullptr || key[0] == '\0') return false;
    std::array<uint8_t, 256U> key_der{};
    size_t key_size = 0;
    if (!DecodeStoreBase64(key, key_der.data(), key_der.size(), key_size)) return false;
    std::array<uint8_t, 72U> der{};
    size_t cursor = 2U;
    for (size_t part = 0; part < 2U; ++part) {
        size_t offset = part * 32U;
        while (offset + 1U < (part + 1U) * 32U && signature[offset] == 0U) ++offset;
        const size_t length = (part + 1U) * 32U - offset;
        const bool padding = (signature[offset] & 0x80U) != 0U;
        der[cursor++] = 2U;
        der[cursor++] = static_cast<uint8_t>(length + (padding ? 1U : 0U));
        if (padding) der[cursor++] = 0U;
        std::memcpy(der.data() + cursor, signature.data() + offset, length);
        cursor += length;
    }
    der[0] = 0x30U;
    der[1] = static_cast<uint8_t>(cursor - 2U);
    std::array<uint8_t, 32U> digest{};
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const uint8_t*>(text.data()), second,
                   digest.data()) != 0)
        return false;
    mbedtls_pk_context public_key;
    mbedtls_pk_init(&public_key);
    const bool verified =
        mbedtls_pk_parse_public_key(&public_key, key_der.data(), key_size) == 0 &&
        mbedtls_pk_verify(&public_key, MBEDTLS_MD_SHA256, digest.data(), digest.size(), der.data(), cursor) == 0;
    mbedtls_pk_free(&public_key);
    if (!verified) return false;
    cJSON* payload =
        cJSON_ParseWithLengthOpts(reinterpret_cast<char*>(payload_bytes.data()), payload_size + 1U, nullptr, true);
    std::array<char, 65U> expected_digest{};
    control::FormatSha256Hex(command.package_sha256, expected_digest);
    const cJSON* size = payload != nullptr ? cJSON_GetObjectItemCaseSensitive(payload, "sizeBytes") : nullptr;
    const char* version = StoreString(payload, "version");
    const bool valid = payload != nullptr && std::strcmp(StoreString(payload, "appId"), command.app_id.data()) == 0 &&
                       std::strcmp(StoreString(payload, "releaseId"), release_id) == 0 &&
                       StoreString(payload, "publisherId")[0] != '\0' &&
                       std::strcmp(StoreString(payload, "sha256"), expected_digest.data()) == 0 &&
                       std::strcmp(StoreString(payload, "target"), "riscv32-ilp32f") == 0 && cJSON_IsNumber(size) &&
                       size->valuedouble == static_cast<double>(command.package_size) && std::strlen(version) > 0U &&
                       std::strlen(version) < command.store_version.size();
    if (valid) {
        std::snprintf(command.store_version.data(), command.store_version.size(), "%s", version);
        command.store_verified = true;
    }
    cJSON_Delete(payload);
    return valid;
}
}  // namespace micropixel::firmware::remote_control
