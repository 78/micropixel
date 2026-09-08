// LinearUpsampler arithmetic and PcmStreamService: ABI validation, ring
// backpressure, mix-rate upsampling through the device PcmSource, low-water
// event delivery and close/teardown.
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/audio/linear_upsampler.hpp"
#include "runtime/audio/pcm_stream_service.hpp"
#include "runtime/event_queue.hpp"

namespace {

constexpr uint32_t kMixRate = 32000U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// Mixer stand-in: records the PcmSource and lets the test pull chunks the way
// the audio task would.
class FakeAudio final : public micropixel::device::Audio {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override {
        info = {};
        info.size = sizeof(info);
        info.sample_rate = kMixRate;
        info.capabilities = MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM;
        info.max_pcm_streams = 1U;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t&) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t StartPcm(const micropixel::device::PcmSource& source, uint32_t token, uint16_t volume,
                                   micropixel::device::PcmStreamHandle& handle_out) override {
        if (active) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        active = true;
        this->source = source;
        this->token = token;
        this->volume = volume;
        handle_out = ++last_handle;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t PausePcm(micropixel::device::PcmStreamHandle) override { return MICROPIXEL_STATUS_OK; }
    [[nodiscard]] int32_t ResumePcm(micropixel::device::PcmStreamHandle) override { return MICROPIXEL_STATUS_OK; }
    [[nodiscard]] int32_t SetPcmVolume(micropixel::device::PcmStreamHandle, uint16_t) override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t StopPcm(micropixel::device::PcmStreamHandle handle) override {
        if (!active || handle != last_handle) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        active = false;
        ++stop_count;
        return MICROPIXEL_STATUS_OK;
    }
    void BindPcmCompletionSink(micropixel::device::PcmCompletionSink, void*) override {}
    void UnbindPcmCompletionSink(void*) override {}
    [[nodiscard]] int32_t StopAll() override { return MICROPIXEL_STATUS_OK; }
    [[nodiscard]] int32_t SuspendAll() override { return MICROPIXEL_STATUS_OK; }
    [[nodiscard]] int32_t ResumeAll() override { return MICROPIXEL_STATUS_OK; }

    // Audio-task stand-in.
    micropixel::device::PcmReadResult Pull(int16_t* out, uint32_t frames) {
        Require(active && source.read != nullptr);
        return source.read(source.context, out, frames);
    }

    bool active{};
    micropixel::device::PcmSource source{};
    uint32_t token{};
    uint16_t volume{};
    micropixel::device::PcmStreamHandle last_handle{};
    uint32_t stop_count{};
};

micropixel_audio_pcm_stream_open_request_t OpenRequest(uint32_t sample_rate, uint16_t channels, uint32_t capacity,
                                                       uint32_t low_water) {
    micropixel_audio_pcm_stream_open_request_t request{};
    request.size = sizeof(request);
    request.volume_per_mille = 800U;
    request.sample_rate = sample_rate;
    request.channels = channels;
    request.capacity_frames = capacity;
    request.low_water_frames = low_water;
    return request;
}

micropixel_audio_pcm_stream_write_request_t WriteRequest(uint32_t stream, uint32_t frames, uint32_t payload_bytes) {
    micropixel_audio_pcm_stream_write_request_t request{};
    request.size = static_cast<uint16_t>(sizeof(request) + payload_bytes);
    request.stream_handle = stream;
    request.frame_count = frames;
    return request;
}

bool TakeEvent(micropixel::runtime::EventQueue& events, micropixel_event_t& event) {
    return events.Wait(event, 0U) == micropixel::runtime::EventWaitResult::kReceived;
}

void TestUpsampler() {
    micropixel::runtime::LinearUpsampler upsampler{};
    upsampler.Reset(2U);
    const int16_t source[3] = {0, 100, -100};
    uint32_t index = 0U;
    int16_t out[8]{};
    const uint32_t produced = upsampler.Produce(out, 8U, [&](int16_t& sample) {
        if (index == 3U) {
            return false;
        }
        sample = source[index++];
        return true;
    });
    // 2x: each source sample yields the midpoint from the previous one and itself.
    Require(produced == 6U);
    Require(out[0] == 0 && out[1] == 0);
    Require(out[2] == 50 && out[3] == 100);
    Require(out[4] == 0 && out[5] == -100);
    Require(upsampler.Idle());

    // A partial group carries across calls.
    upsampler.Reset(4U);
    index = 0U;
    const int16_t single[1] = {400};
    Require(upsampler.Produce(out, 3U, [&](int16_t& sample) {
        if (index == 1U) {
            return false;
        }
        sample = single[index++];
        return true;
    }) == 3U);
    Require(out[0] == 100 && out[1] == 200 && out[2] == 300);
    Require(!upsampler.Idle());
    Require(upsampler.Produce(out, 3U, [&](int16_t&) { return false; }) == 1U);
    Require(out[0] == 400);
    Require(upsampler.Idle());
}

void TestOpenValidation() {
    FakeAudio audio{};
    micropixel::device::AudioService audio_service{audio};
    micropixel::runtime::EventQueue events{};
    micropixel::runtime::PcmStreamService service{audio_service, events, 0};
    Require(service.valid());

    auto request = OpenRequest(16000U, 1U, 4096U, 1024U);
    request.size = 4U;
    Require(!service.Open(request) && service.Open(request).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    request = OpenRequest(16000U, 3U, 4096U, 1024U);
    Require(service.Open(request).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    request = OpenRequest(16000U, 1U, 4096U, 4096U);
    Require(service.Open(request).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    request = OpenRequest(16000U, 1U, micropixel::runtime::PcmStreamService::kMaxCapacityFrames + 1U, 0U);
    Require(service.Open(request).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    // 22050 does not divide 32000; 48000 exceeds it.
    Require(service.Open(OpenRequest(22050U, 1U, 4096U, 0U)).error().status == MICROPIXEL_STATUS_UNSUPPORTED);
    Require(service.Open(OpenRequest(48000U, 1U, 4096U, 0U)).error().status == MICROPIXEL_STATUS_UNSUPPORTED);
    Require(!audio.active);

    auto opened = service.Open(OpenRequest(kMixRate, 1U, 1024U, 0U));
    Require(opened && opened->stream_handle != 0U && opened->capacity_frames == 1024U);
    Require(audio.active && audio.volume == 800U);
    // Only one stream per session.
    Require(service.Open(OpenRequest(kMixRate, 1U, 1024U, 0U)).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    Require(service.Close(opened->stream_handle + 1U).error().status == MICROPIXEL_STATUS_NOT_FOUND);
    Require(service.Close(opened->stream_handle).has_value());
    Require(!audio.active && audio.stop_count == 1U);
    Require(service.Close(opened->stream_handle).error().status == MICROPIXEL_STATUS_NOT_FOUND);
}

void TestWriteBackpressureAndPassThrough() {
    FakeAudio audio{};
    micropixel::device::AudioService audio_service{audio};
    micropixel::runtime::EventQueue events{};
    micropixel::runtime::PcmStreamService service{audio_service, events, 0};
    auto opened = service.Open(OpenRequest(kMixRate, 1U, 256U, 0U));
    Require(opened.has_value());
    const uint32_t stream = opened->stream_handle;

    std::vector<int16_t> samples(300U);
    for (uint32_t index = 0U; index < samples.size(); ++index) {
        samples[index] = static_cast<int16_t>(index);
    }
    // Payload length must match frame_count * channels.
    Require(service.Write(WriteRequest(stream, 10U, 10U * sizeof(int16_t)), samples.data(), 9U * sizeof(int16_t))
                .error()
                .status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    Require(service.Write(WriteRequest(stream + 1U, 10U, 10U * sizeof(int16_t)), samples.data(), 10U * sizeof(int16_t))
                .error()
                .status == MICROPIXEL_STATUS_NOT_FOUND);

    // 300 frames into a 256-frame ring: only the prefix is accepted.
    auto written = service.Write(WriteRequest(stream, 300U, 600U), samples.data(), 600U);
    Require(written && written->accepted_frames == 256U && written->free_frames == 0U);
    written = service.Write(WriteRequest(stream, 10U, 20U), samples.data(), 20U);
    Require(written && written->accepted_frames == 0U && written->free_frames == 0U);

    int16_t out[128]{};
    auto read = audio.Pull(out, 128U);
    Require(read.frames == 128U && !read.finished);
    for (uint32_t index = 0U; index < 128U; ++index) {
        Require(out[index] == static_cast<int16_t>(index));
    }
    // Ring wrap: the freed frames are reusable and come out in order.
    written = service.Write(WriteRequest(stream, 300U, 600U), samples.data() + 256U, 88U);
    Require(!written.has_value());  // payload mismatch guard still applies
    written = service.Write(WriteRequest(stream, 44U, 88U), samples.data() + 256U, 88U);
    Require(written && written->accepted_frames == 44U && written->free_frames == 84U);
    read = audio.Pull(out, 128U);
    Require(read.frames == 128U);
    for (uint32_t index = 0U; index < 128U; ++index) {
        Require(out[index] == static_cast<int16_t>(128U + index));
    }
    read = audio.Pull(out, 128U);
    Require(read.frames == 44U && !read.finished);  // underrun: short, not finished
    Require(out[0] == 256 && out[43] == 299);
    read = audio.Pull(out, 128U);
    Require(read.frames == 0U && !read.finished);
    service.Shutdown();
    Require(!audio.active);
}

void TestUpsampledStereoAndLowWater() {
    FakeAudio audio{};
    micropixel::device::AudioService audio_service{audio};
    micropixel::runtime::EventQueue events{};
    micropixel::runtime::PcmStreamService service{audio_service, events, 0};
    // 16 kHz stereo into a 32 kHz mono mixer, low-water at 64 source frames.
    auto opened = service.Open(OpenRequest(16000U, 2U, 512U, 64U));
    Require(opened && opened->capacity_frames == 512U);
    const uint32_t stream = opened->stream_handle;

    // Left = 200, right = 400 -> mono 300 for every frame.
    std::vector<int16_t> stereo(2U * 128U);
    for (uint32_t frame = 0U; frame < 128U; ++frame) {
        stereo[frame * 2U] = 200;
        stereo[frame * 2U + 1U] = 400;
    }
    auto written = service.Write(WriteRequest(stream, 128U, 512U), stereo.data(), 512U);
    Require(written && written->accepted_frames == 128U && written->free_frames == 384U);

    // Each 16 kHz frame becomes two 32 kHz frames, so one 128-frame mixer
    // chunk consumes 64 source frames and leaves exactly low_water buffered.
    int16_t out[128]{};
    auto read = audio.Pull(out, 128U);
    Require(read.frames == 128U && !read.finished);
    Require(out[0] == 150 && out[1] == 300);  // ramp from silence into the first sample
    Require(out[2] == 300 && out[127] == 300);
    micropixel_event_t event{};
    Require(TakeEvent(events, event));
    Require(event.service_id == MICROPIXEL_SERVICE_AUDIO &&
            event.event_id == MICROPIXEL_AUDIO_EVENT_PCM_STREAM_LOW_WATER && event.source == stream &&
            event.status == MICROPIXEL_STATUS_OK);
    micropixel_audio_pcm_event_payload_t payload{};
    std::memcpy(&payload, event.payload, sizeof(payload));
    Require(payload.stream_handle == stream && payload.free_frames == 512U - 64U);
    // Delivered once per crossing: draining further does not repeat it.
    read = audio.Pull(out, 128U);
    Require(read.frames == 128U);
    read = audio.Pull(out, 128U);
    Require(read.frames == 0U && !read.finished);
    Require(!TakeEvent(events, event));
    // A new write re-arms it.
    written = service.Write(WriteRequest(stream, 128U, 512U), stereo.data(), 512U);
    Require(written && written->accepted_frames == 128U);
    read = audio.Pull(out, 128U);
    Require(read.frames == 128U);
    Require(TakeEvent(events, event) && event.event_id == MICROPIXEL_AUDIO_EVENT_PCM_STREAM_LOW_WATER);
    Require(!TakeEvent(events, event));

    // CloseAll (STOP_ALL / teardown) stops the device voice.
    service.CloseAll();
    Require(!audio.active && audio.stop_count == 1U);
    Require(service.Write(WriteRequest(stream, 1U, 4U), stereo.data(), 4U).error().status ==
            MICROPIXEL_STATUS_NOT_FOUND);
}

}  // namespace

int main() {
    TestUpsampler();
    TestOpenValidation();
    TestWriteBackpressureAndPassThrough();
    TestUpsampledStereoAndLowWater();
    return 0;
}
