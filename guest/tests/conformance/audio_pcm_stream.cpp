// Audio 1.2 Guest PCM stream through the SDK: opens a stream at the mix rate
// and at half of it, streams a sine through ring backpressure, receives the
// low-water event and closes cleanly. Exit codes 50..59 name the failed step.
#include <type_traits>

#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

namespace {

static_assert(std::is_move_constructible_v<micropixel::PcmStream>);
static_assert(!std::is_copy_constructible_v<micropixel::PcmStream>);

constexpr uint32_t kToneHz = 440U;
constexpr uint32_t kBlockFrames = 512U;
constexpr uint32_t kTotalBlocks = 24U;

// Quarter-wave sine table is enough for a test tone; avoids libm in the Guest.
constexpr int16_t kQuarterSine[17] = {0,     3212,  6393,  9512,  12539, 15446, 18204, 20787, 23170,
                                      25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767};

int16_t Sine(uint32_t phase_turns_16) {
    // phase in 1/65536 turns
    const uint32_t quadrant = (phase_turns_16 >> 14U) & 3U;
    uint32_t index = (phase_turns_16 >> 10U) & 15U;
    if (quadrant & 1U) {
        index = 16U - index;
    }
    const int16_t value = kQuarterSine[index];
    return quadrant >= 2U ? static_cast<int16_t>(-value) : value;
}

void FillBlock(int16_t* block, uint32_t frames, uint32_t sample_rate, uint32_t& phase) {
    const uint32_t step = static_cast<uint32_t>((static_cast<uint64_t>(kToneHz) << 16U) / sample_rate);
    for (uint32_t frame = 0U; frame < frames; ++frame) {
        block[frame] = static_cast<int16_t>(Sine(phase) / 4);
        phase = (phase + step) & 0xffffU;
    }
}

}  // namespace

int main() {
    micropixel::Application app;
    micropixel::Audio audio = app.audio();
    auto info = audio.info();
    if (!info || !info->supports_pcm_stream || info->max_pcm_streams == 0U) {
        app.log().Error("audio_pcm_stream: Audio 1.2 PCM stream capability missing");
        return 50;
    }
    const uint32_t mix_rate = info->sample_rate;

    // Invalid options are rejected locally and by the Host alike.
    if (audio.OpenPcmStream({.sample_rate = mix_rate, .channels = 3U}) ||
        audio.OpenPcmStream({.sample_rate = mix_rate, .capacity_frames = 1024U, .low_water_frames = 1024U}) ||
        audio.OpenPcmStream({.sample_rate = mix_rate + 1U})) {
        return 51;
    }

    auto opened = audio.OpenPcmStream({
        .sample_rate = mix_rate,
        .channels = 1U,
        .capacity_frames = 2048U,
        .low_water_frames = 1024U,
        .volume_per_mille = 300U,
    });
    if (!opened) {
        app.log().Error("audio_pcm_stream: open at mix rate failed");
        return 52;
    }
    micropixel::PcmStream stream = static_cast<micropixel::PcmStream&&>(*opened);
    if (!stream.valid() || stream.sample_rate() != mix_rate || stream.capacity_frames() < 2048U) {
        return 53;
    }
    // Only one stream at a time.
    if (audio.OpenPcmStream({.sample_rate = mix_rate})) {
        return 54;
    }

    alignas(4) int16_t block[kBlockFrames]{};
    uint32_t phase = 0U;
    uint32_t blocks_done = 0U;
    uint32_t pending = 0U;  // frames of `block` still to be accepted
    uint32_t low_water_events = 0U;
    bool saw_backpressure = false;
    while (blocks_done < kTotalBlocks) {
        if (pending == 0U) {
            FillBlock(block, kBlockFrames, mix_rate, phase);
            pending = kBlockFrames;
        }
        auto accepted = stream.Write(block + (kBlockFrames - pending), pending);
        if (!accepted) {
            app.log().Error("audio_pcm_stream: write failed");
            return 55;
        }
        if (*accepted > pending) {
            return 56;
        }
        pending -= *accepted;
        if (pending == 0U) {
            ++blocks_done;
            continue;
        }
        // Ring full: wait for the Host to drain to the low-water mark.
        saw_backpressure = true;
        micropixel::Event event;
        if (!app.WaitEventFor(event, micropixel::Duration::Seconds(2U))) {
            app.log().Error("audio_pcm_stream: low-water event timed out");
            return 57;
        }
        if (event.type() == micropixel::EventType::kStop) {
            return 58;
        }
        if (const micropixel::PcmStreamEvent* low = event.LowWaterFrom(stream); low != nullptr) {
            if (low->free_frames() < 1024U) {
                return 59;
            }
            ++low_water_events;
        }
    }
    if (!saw_backpressure || low_water_events == 0U) {
        app.log().Error("audio_pcm_stream: never hit ring backpressure; capacity too large for the test");
        return 60;
    }
    if (!stream.Close() || stream.valid()) {
        return 61;
    }
    // Closed handle: further use is a local state error, not a Host trip.
    if (stream.Write(block, 1U) || stream.Write(block, 1U).error().code() != micropixel::ErrorCode::kInvalidState) {
        return 62;
    }

    // Half the mix rate, stereo: the Host upsamples 2x and downmixes.
    auto half = audio.OpenPcmStream({
        .sample_rate = mix_rate / 2U,
        .channels = 2U,
        .capacity_frames = 1024U,
        .low_water_frames = 0U,
        .volume_per_mille = 300U,
    });
    if (!half) {
        return 63;
    }
    alignas(4) int16_t stereo[kBlockFrames * 2U]{};
    phase = 0U;
    FillBlock(block, kBlockFrames, mix_rate / 2U, phase);
    for (uint32_t frame = 0U; frame < kBlockFrames; ++frame) {
        stereo[frame * 2U] = block[frame];
        stereo[frame * 2U + 1U] = static_cast<int16_t>(-block[frame] / 2);
    }
    auto stereo_written = half->Write(stereo, kBlockFrames);
    if (!stereo_written || *stereo_written != kBlockFrames || half->free_frames() != 1024U - kBlockFrames) {
        return 64;
    }
    // Let it play out before destruction closes the stream.
    micropixel::Timer timer = app.timers().After(micropixel::Duration::Milliseconds(120U));
    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (event.TimerFrom(timer) != nullptr) {
            break;
        }
        if (event.type() == micropixel::EventType::kStop) {
            return 65;
        }
    }
    half->Reset();
    if (half->valid()) {
        return 66;
    }

    app.log().Info("audio_pcm_stream: streamed with backpressure, low-water and 2x upsampled stereo");
    return 0;
}
