#include "host/controller/remote/remote_control_protocol.hpp"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_random.h"

namespace micropixel::firmware::remote_control::protocol {
namespace {

bool IsHex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

const char* JsonString(const cJSON* root, const char* name) {
    const cJSON* value = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, name) : nullptr;
    return cJSON_IsString(value) ? cJSON_GetStringValue(value) : nullptr;
}

}  // namespace

void GenerateUuid(Uuid& output) {
    std::array<uint8_t, 16U> bytes{};
    for (size_t offset = 0U; offset < bytes.size(); offset += sizeof(uint32_t)) {
        const uint32_t random = esp_random();
        std::memcpy(bytes.data() + offset, &random, sizeof(random));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    std::snprintf(output.data(), output.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                  bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool IsUuid(const char* value) {
    if (value == nullptr || std::strlen(value) != 36U) {
        return false;
    }
    for (size_t index = 0U; index < 36U; ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return false;
        } else if (!IsHex(value[index])) {
            return false;
        }
    }
    return true;
}

bool ParseSessionReady(const cJSON* root, Uuid& session_id_out) {
    const cJSON* version = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "protocolVersion") : nullptr;
    const char* type = JsonString(root, "type");
    const char* session_id = JsonString(root, "sessionId");
    if (!cJSON_IsNumber(version) || version->valuedouble != static_cast<double>(kVersion) || type == nullptr ||
        std::strcmp(type, "session.ready") != 0 || !IsUuid(session_id)) {
        return false;
    }
    std::snprintf(session_id_out.data(), session_id_out.size(), "%s", session_id);
    return true;
}

bool ValidateCommandEnvelope(const cJSON* root, const Uuid& session_id) {
    const cJSON* version = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "protocolVersion") : nullptr;
    const char* type = JsonString(root, "type");
    const char* frame_session_id = JsonString(root, "sessionId");
    const char* command_id = JsonString(root, "commandId");
    const char* name = JsonString(root, "name");
    const cJSON* params = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "params") : nullptr;
    return cJSON_IsNumber(version) && version->valuedouble == static_cast<double>(kVersion) && session_id[0] != '\0' &&
           type != nullptr && std::strcmp(type, "command") == 0 && frame_session_id != nullptr &&
           std::strcmp(frame_session_id, session_id.data()) == 0 && IsUuid(command_id) && name != nullptr &&
           name[0] != '\0' && std::strlen(name) < 64U && cJSON_IsObject(params);
}

bool AddEventEnvelope(cJSON* root, const char* type, const Uuid& session_id, const Uuid& device_boot_id,
                      uint64_t event_sequence, uint64_t occurred_at_uptime_ms) {
    if (root == nullptr || type == nullptr || !IsUuid(session_id.data()) || !IsUuid(device_boot_id.data()) ||
        event_sequence == 0U) {
        return false;
    }
    Uuid event_id{};
    GenerateUuid(event_id);
    return cJSON_AddNumberToObject(root, "protocolVersion", kVersion) != nullptr &&
           cJSON_AddStringToObject(root, "type", type) != nullptr &&
           cJSON_AddStringToObject(root, "sessionId", session_id.data()) != nullptr &&
           cJSON_AddStringToObject(root, "deviceBootId", device_boot_id.data()) != nullptr &&
           cJSON_AddStringToObject(root, "eventId", event_id.data()) != nullptr &&
           cJSON_AddNumberToObject(root, "eventSequence", static_cast<double>(event_sequence)) != nullptr &&
           cJSON_AddNumberToObject(root, "occurredAtUptimeMs", static_cast<double>(occurred_at_uptime_ms)) != nullptr;
}

void AddProtocolError(cJSON* parent, const char* code, const char* message, bool retryable) {
    cJSON* error = parent != nullptr ? cJSON_AddObjectToObject(parent, "error") : nullptr;
    if (error == nullptr) return;
    (void)cJSON_AddStringToObject(error, "code", code != nullptr ? code : "unknown_error");
    (void)cJSON_AddStringToObject(error, "message", message != nullptr ? message : "Unknown device error.");
    (void)cJSON_AddBoolToObject(error, "retryable", retryable);
    (void)cJSON_AddObjectToObject(error, "details");
}

}  // namespace micropixel::firmware::remote_control::protocol
