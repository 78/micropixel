#include <cstdio>
#include <vector>

#include "host_ui/system_gesture_router.hpp"

namespace {

using micropixel::device::InputBackend;
using micropixel::device::TouchPhase;
using micropixel::device::TouchSample;
using micropixel::device::TouchSink;
using micropixel::host_ui::SystemGestureRouter;
using micropixel::host_ui::SystemUiAction;
using micropixel::host_ui::SystemUiActionType;

class FakeInputBackend final : public InputBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override {
        info = {};
        return 0;
    }

    void BindTouchSink(TouchSink sink, void* context) override {
        sink_ = sink;
        context_ = context;
    }

    void UnbindTouchSink(void* context) override {
        if (context_ == context) {
            sink_ = nullptr;
            context_ = nullptr;
        }
    }

    [[nodiscard]] bool Emit(const TouchSample& sample) const { return sink_ != nullptr && sink_(context_, sample); }

   private:
    TouchSink sink_{};
    void* context_{};
};

struct Capture final {
    std::vector<TouchSample> guest_samples;
    std::vector<SystemUiAction> system_actions;
};

bool CaptureGuest(void* context, const TouchSample& sample) {
    static_cast<Capture*>(context)->guest_samples.push_back(sample);
    return true;
}

void CaptureSystem(void* context, const SystemUiAction& action) {
    static_cast<Capture*>(context)->system_actions.push_back(action);
}

TouchSample Sample(TouchPhase phase, uint16_t x, uint16_t y, uint64_t timestamp_us, uint32_t id = 1U) {
    return TouchSample{.timestamp_us = timestamp_us, .id = id, .x = x, .y = y, .pressure = 500U, .phase = phase};
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

bool NormalTouchPassesThrough() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 120U, 200U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 126U, 208U, 10000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 126U, 208U, 20000U));
    return Check(capture.guest_samples.size() == 3U, "ordinary touch sequence must reach Guest") &&
           Check(capture.system_actions.empty(), "ordinary touch must not emit a System action");
}

bool TopGestureIsReserved() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 300U, 48U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 304U, 110U, 100000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 304U, 110U, 120000U));
    return Check(capture.guest_samples.empty(), "recognized top gesture must not leak to Guest") &&
           Check(capture.system_actions.size() == 1U, "top gesture must emit exactly one System action") &&
           Check(capture.system_actions[0].type == SystemUiActionType::kOpenStatusLayer,
                 "top gesture must open the status layer");
}

bool BottomGestureIsReserved() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 360U, 710U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 358U, 610U, 90000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 358U, 610U, 110000U));
    return Check(capture.guest_samples.empty(), "recognized bottom gesture must not leak to Guest") &&
           Check(capture.system_actions.size() == 1U, "bottom gesture must emit exactly one System action") &&
           Check(capture.system_actions[0].type == SystemUiActionType::kSuspendToHall,
                 "bottom gesture must suspend to Hall");
}

bool BottomGestureRejectsIncidentalMovement() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    // A swipe beginning outside the final 32 rows belongs to the Guest even
    // when it travels far enough to resemble the old System gesture.
    (void)input.Emit(Sample(TouchPhase::kDown, 360U, 680U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 358U, 580U, 90000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 358U, 580U, 110000U));
    if (!Check(capture.guest_samples.size() == 3U,
               "bottom swipe outside the reserved 32 rows must reach Guest") ||
        !Check(capture.system_actions.empty(), "bottom swipe outside reserved rows must not suspend")) {
        return false;
    }

    capture = {};
    // A short movement from the physical edge is also insufficient; the
    // deliberate System gesture must travel at least 96 pixels upward.
    (void)input.Emit(Sample(TouchPhase::kDown, 360U, 710U, 200000U));
    (void)input.Emit(Sample(TouchPhase::kMove, 358U, 646U, 290000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 358U, 646U, 310000U));
    return Check(capture.system_actions.empty(), "short bottom-edge movement must not suspend");
}

bool RecognizedGestureQuarantinesReplacementTrack() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture guest_capture;
    Capture hall_capture;
    router.BindTouchSink(CaptureGuest, &guest_capture);
    router.BindSystemActionSink(CaptureSystem, &guest_capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 180U, 710U, 0U, 3U));
    (void)input.Emit(Sample(TouchPhase::kMove, 180U, 600U, 90000U, 3U));
    router.BindTouchSink(CaptureGuest, &hall_capture);

    // GT911 reports a replacement track before synthesizing Up for the old
    // one. None of this continuing physical gesture may reach the Hall.
    (void)input.Emit(Sample(TouchPhase::kDown, 180U, 420U, 100000U, 4U));
    (void)input.Emit(Sample(TouchPhase::kUp, 180U, 600U, 100000U, 3U));
    (void)input.Emit(Sample(TouchPhase::kMove, 180U, 400U, 110000U, 4U));
    (void)input.Emit(Sample(TouchPhase::kUp, 180U, 400U, 120000U, 4U));
    if (!Check(hall_capture.guest_samples.empty(),
               "replacement track from recognized gesture must not leak into newly bound Hall")) {
        return false;
    }

    // The next independent touch is a valid Hall interaction.
    (void)input.Emit(Sample(TouchPhase::kDown, 180U, 420U, 200000U, 5U));
    (void)input.Emit(Sample(TouchPhase::kUp, 180U, 420U, 220000U, 5U));
    if (!Check(hall_capture.guest_samples.empty(),
               "touch bounce immediately after gesture release must remain quarantined")) {
        return false;
    }
    (void)input.Emit(Sample(TouchPhase::kDown, 180U, 420U, 500000U, 6U));
    (void)input.Emit(Sample(TouchPhase::kUp, 180U, 420U, 520000U, 6U));
    return Check(hall_capture.guest_samples.size() == 2U,
                 "first independent touch after release must reach newly bound Hall") &&
           Check(guest_capture.system_actions.size() == 1U,
                 "replacement track must not emit another System action");
}

bool RejectedEdgeGestureReplaysCompleteSequence() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 100U, 8U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 145U, 18U, 50000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 150U, 22U, 70000U));
    return Check(capture.guest_samples.size() == 3U, "rejected edge touch must replay Down/Move/Up") &&
           Check(capture.guest_samples[0].phase == TouchPhase::kDown, "replayed sequence must begin with Down") &&
           Check(capture.guest_samples[1].phase == TouchPhase::kMove, "replayed sequence must retain rejecting Move") &&
           Check(capture.guest_samples[2].phase == TouchPhase::kUp, "replayed sequence must end with Up") &&
           Check(capture.system_actions.empty(), "rejected edge touch must not emit a System action");
}

bool ReleasedEdgeTapReplaysDownAndUp() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 400U, 712U, 0U));
    (void)input.Emit(Sample(TouchPhase::kUp, 401U, 711U, 40000U));
    return Check(capture.guest_samples.size() == 2U, "edge tap must replay a complete Down/Up pair") &&
           Check(capture.guest_samples[0].phase == TouchPhase::kDown, "edge tap replay must begin with Down") &&
           Check(capture.guest_samples[1].phase == TouchPhase::kUp, "edge tap replay must end with Up") &&
           Check(capture.system_actions.empty(), "edge tap must not emit a System action");
}

bool RejectedEdgeDragRetainsLatestMove() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 400U, 712U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 402U, 694U, 20000U));
    (void)input.Emit(Sample(TouchPhase::kMove, 404U, 680U, 40000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 405U, 678U, 60000U));
    return Check(capture.guest_samples.size() == 3U,
                 "rejected edge drag must replay Down/latest Move/Up") &&
           Check(capture.guest_samples[0].phase == TouchPhase::kDown,
                 "rejected edge drag must begin with Down") &&
           Check(capture.guest_samples[1].phase == TouchPhase::kMove &&
                     capture.guest_samples[1].x == 404U && capture.guest_samples[1].y == 680U,
                 "rejected edge drag must retain its latest pending Move") &&
           Check(capture.guest_samples[2].phase == TouchPhase::kUp,
                 "rejected edge drag must end with Up") &&
           Check(capture.system_actions.empty(), "rejected edge drag must not emit a System action");
}

bool TimedOutCandidateReturnsToGuest() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 200U, 6U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 201U, 30U, 700000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 201U, 30U, 710000U));
    return Check(capture.guest_samples.size() == 3U, "timed-out edge candidate must return to Guest") &&
           Check(capture.system_actions.empty(), "timed-out edge candidate must not emit a System action");
}

}  // namespace

int main() {
    const bool passed = NormalTouchPassesThrough() && TopGestureIsReserved() && BottomGestureIsReserved() &&
                        BottomGestureRejectsIncidentalMovement() && RecognizedGestureQuarantinesReplacementTrack() &&
                        RejectedEdgeGestureReplaysCompleteSequence() && ReleasedEdgeTapReplaysDownAndUp() &&
                        RejectedEdgeDragRetainsLatestMove() && TimedOutCandidateReturnsToGuest();
    if (!passed) {
        return 1;
    }
    std::puts("SystemGestureRouter host tests passed (9 cases).");
    return 0;
}
