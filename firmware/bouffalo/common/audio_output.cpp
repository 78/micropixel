// SPDX-License-Identifier: Apache-2.0
#include "audio_output.hpp"
extern "C" {
#include "audio_service.h"
#include "wave_player.h"
}
namespace mpx_port {
esp_err_t AudioOutput::Initialize() { return audio_service_init() ? ESP_OK : ESP_FAIL; }
esp_err_t AudioOutput::Start(int32_t*, uint32_t) {
    wave_player_config(48000, WAVE_PLAYER_OUT_SAMPLES * 4U, 0, 2);
    wave_player_set_volume(65);
    wave_player_set_mute(false);
    return wave_player_start() == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t AudioOutput::Write(const int32_t* p, uint32_t n) {
    while (n) {
        uint32_t frames = n > 128 ? 128 : n;
        for (uint32_t i = 0; i < frames * 2; ++i) samples_[i] = p[i] >> 16;
        uint32_t bytes = frames * 4;
        if (wave_player_feed_blocking((uint8_t*)samples_, bytes) != bytes) return ESP_FAIL;
        p += frames * 2;
        n -= frames;
    }
    return ESP_OK;
}
esp_err_t AudioOutput::Stop() { return wave_player_stop() == 0 ? ESP_OK : ESP_FAIL; }
void AudioOutput::Shutdown() { audio_service_shutdown(); }
}  // namespace mpx_port
