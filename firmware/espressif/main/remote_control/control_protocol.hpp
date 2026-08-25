#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_PROTOCOL_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>

struct cJSON;

namespace micropixel::firmware::remote_control::protocol {

constexpr uint32_t kVersion = 1U;
constexpr size_t kUuidCapacity = 37U;
using Uuid = std::array<char, kUuidCapacity>;

void GenerateUuid(Uuid& output);
[[nodiscard]] bool IsUuid(const char* value);
[[nodiscard]] bool ParseSessionReady(const cJSON* root, Uuid& session_id_out);
[[nodiscard]] bool ValidateCommandEnvelope(const cJSON* root, const Uuid& session_id);
[[nodiscard]] bool AddEventEnvelope(cJSON* root, const char* type, const Uuid& session_id, const Uuid& device_boot_id,
                                    uint64_t event_sequence, uint64_t occurred_at_uptime_ms);
void AddProtocolError(cJSON* parent, const char* code, const char* message, bool retryable);

}  // namespace micropixel::firmware::remote_control::protocol

#endif
