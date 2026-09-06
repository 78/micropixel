// GET_INFO responses are append-only. A Guest built against an older header
// hands the Host a shorter buffer and must still receive the prefix it knows,
// so installed Apps keep running after a Host minor upgrade. Exit codes 50..63
// name the failed step.
#include <stdint.h>

#include "abi/micropixel_abi.h"

namespace {

// Graphics 1.1 knew 40 bytes (safe insets included); 1.0 knew 32.
constexpr uint32_t kGraphics11Size = 40U;
constexpr uint32_t kAudio11Size = MICROPIXEL_AUDIO_INFO_MIN_SIZE;

alignas(8) uint8_t g_full[128];
alignas(8) uint8_t g_short[128];

bool Open(uint32_t service_id, uint32_t version, uint32_t& handle) {
    micropixel_service_info_t service{};
    if (micropixel_service_open(service_id, version, &service, sizeof(service)) != MICROPIXEL_STATUS_OK) {
        return false;
    }
    handle = service.handle;
    return true;
}

int32_t GetInfo(uint32_t handle, uint32_t method, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = 0U;
    return micropixel_service_call(handle, method, nullptr, 0U, response, capacity, &size);
}

bool SamePrefix(uint32_t bytes) {
    for (uint32_t index = 0U; index < bytes; ++index) {
        if (g_full[index] != g_short[index]) {
            return false;
        }
    }
    return true;
}

uint16_t ReadU16(const uint8_t* bytes) { return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)); }

}  // namespace

int main() {
    uint32_t graphics = 0U;
    if (!Open(MICROPIXEL_SERVICE_GRAPHICS,
              MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
              graphics)) {
        return 50;
    }
    uint32_t size = 0U;
    if (GetInfo(graphics, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, g_full, sizeof(g_full), size) != MICROPIXEL_STATUS_OK ||
        size != sizeof(micropixel_graphics_info_t) || ReadU16(g_full) != sizeof(micropixel_graphics_info_t)) {
        return 51;
    }
    // Older Guest: shorter buffer gets the known prefix, `size` still reports
    // the Host's full structure so a newer Guest can detect the tail fields.
    if (GetInfo(graphics, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, g_short, kGraphics11Size, size) != MICROPIXEL_STATUS_OK ||
        size != kGraphics11Size || !SamePrefix(kGraphics11Size)) {
        return 52;
    }
    if (GetInfo(graphics, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, g_short, MICROPIXEL_GRAPHICS_INFO_MIN_SIZE, size) !=
            MICROPIXEL_STATUS_OK ||
        size != MICROPIXEL_GRAPHICS_INFO_MIN_SIZE || !SamePrefix(MICROPIXEL_GRAPHICS_INFO_MIN_SIZE)) {
        return 53;
    }
    // Below the first published version there is no meaningful prefix.
    if (GetInfo(graphics, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, g_short, MICROPIXEL_GRAPHICS_INFO_MIN_SIZE - 4U, size) !=
            MICROPIXEL_STATUS_BUFFER_TOO_SMALL ||
        size != sizeof(micropixel_graphics_info_t)) {
        return 54;
    }
    // Oversized buffers still receive exactly the Host structure.
    if (GetInfo(graphics, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, g_short, sizeof(g_short), size) != MICROPIXEL_STATUS_OK ||
        size != sizeof(micropixel_graphics_info_t) || !SamePrefix(sizeof(micropixel_graphics_info_t))) {
        return 55;
    }

    uint32_t audio = 0U;
    if (!Open(MICROPIXEL_SERVICE_AUDIO,
              MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_AUDIO_INTERFACE_MAJOR, MICROPIXEL_AUDIO_INTERFACE_MINOR), audio)) {
        return 60;
    }
    if (GetInfo(audio, MICROPIXEL_AUDIO_METHOD_GET_INFO, g_full, sizeof(g_full), size) != MICROPIXEL_STATUS_OK ||
        size != sizeof(micropixel_audio_info_t) || ReadU16(g_full) != sizeof(micropixel_audio_info_t)) {
        return 61;
    }
    if (GetInfo(audio, MICROPIXEL_AUDIO_METHOD_GET_INFO, g_short, kAudio11Size, size) != MICROPIXEL_STATUS_OK ||
        size != kAudio11Size || !SamePrefix(kAudio11Size)) {
        return 62;
    }
    if (GetInfo(audio, MICROPIXEL_AUDIO_METHOD_GET_INFO, g_short, kAudio11Size - 4U, size) !=
            MICROPIXEL_STATUS_BUFFER_TOO_SMALL ||
        size != sizeof(micropixel_audio_info_t)) {
        return 63;
    }
    return 0;
}
