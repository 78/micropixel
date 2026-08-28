#include <cstdio>
#include <vector>

#include "host_ui/system_gesture_router.hpp"

namespace {

using micropixel::device::InputBackend;
using micropixel::device::KeyCode;
using micropixel::device::KeyPhase;
using micropixel::device::KeySample;
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

    [[nodiscard]] bool InjectTouch(const TouchSample& sample) override {
        ++injected_count_;
        return Emit(sample);
    }

    [[nodiscard]] bool Emit(const TouchSample& sample) const { return sink_ != nullptr && sink_(context_, sample); }
    [[nodiscard]] uint32_t injected_count() const { return injected_count_; }

   private:
    TouchSink sink_{};
    void* context_{};
    uint32_t injected_count_{};
};

struct Capture final {
    std::vector<TouchSample> guest_samples;
    std::vector<KeySample> guest_keys;
    std::vector<SystemUiAction> system_actions;
    uint32_t activity_count{};
};

bool CaptureGuest(void* context, const TouchSample& sample) {
    static_cast<Capture*>(context)->guest_samples.push_back(sample);
    return true;
}

bool CaptureGuestKey(void* context, const KeySample& sample) {
    static_cast<Capture*>(context)->guest_keys.push_back(sample);
    return true;
}

void CaptureSystem(void* context, const SystemUiAction& action) {
    static_cast<Capture*>(context)->system_actions.push_back(action);
}

void CaptureActivity(void* context) { ++static_cast<Capture*>(context)->activity_count; }

TouchSample Sample(TouchPhase phase, uint16_t x, uint16_t y, uint64_t timestamp_us, uint32_t id = 1U) {
    return TouchSample{
        .timestamp_us = timestamp_us,
        .id = id,
        .x = x,
        .y = y,
        .pressure_per_mille = 500U,
        .phase = phase,
    };
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

bool InjectedTouchTraversesPlatformAndSystemRouter() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);

    const bool injected = router.InjectTouch(Sample(TouchPhase::kDown, 120U, 200U, 0U));
    return Check(injected, "virtual touch must be accepted by the platform backend") &&
           Check(input.injected_count() == 1U, "virtual touch must traverse the platform backend") &&
           Check(capture.guest_samples.size() == 1U, "virtual touch must return through the System gesture router");
}

bool SemanticKeyReachesGuest() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.BindKeySink(CaptureGuestKey, &capture);

    micropixel_input_info_t info{};
    const KeySample sample{
        .timestamp_us = 42U,
        .code = KeyCode::kConfirm,
        .phase = KeyPhase::kDown,
    };
    const bool injected = router.InjectKey(sample);
    return Check(router.GetInfo(info) == MICROPIXEL_STATUS_OK, "logical input info must remain available") &&
           Check(info.interface_major == MICROPIXEL_INPUT_INTERFACE_MAJOR &&
                     info.interface_minor == MICROPIXEL_INPUT_INTERFACE_MINOR,
                 "logical input must advertise the current Input interface") &&
           Check((info.capabilities & MICROPIXEL_INPUT_CAP_KEY_EVENTS) != 0U,
                 "logical input must advertise semantic key events") &&
           Check(injected, "semantic key must be accepted while a Guest sink is bound") &&
           Check(capture.guest_keys.size() == 1U && capture.guest_keys[0].code == KeyCode::kConfirm,
                 "semantic key must reach the Guest key sink");
}

bool TouchAndKeyInputReportUserActivity() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 720U, 720U);
    Capture capture;
    router.SetActivitySink(CaptureActivity, &capture);

    (void)input.Emit(Sample(TouchPhase::kDown, 100U, 100U, 1U));
    (void)router.InjectKey(KeySample{.timestamp_us = 2U, .code = KeyCode::kConfirm, .phase = KeyPhase::kDown});
    if (!Check(capture.activity_count == 2U, "touch and semantic key input must both report user activity")) {
        return false;
    }
    router.SetActivitySink(nullptr, nullptr);
    (void)input.Emit(Sample(TouchPhase::kUp, 100U, 100U, 3U));
    return Check(capture.activity_count == 2U, "clearing the activity sink must stop notifications");
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

bool TopGestureScalesFor480Display() {
    FakeInputBackend input;
    SystemGestureRouter router(input, 480U, 480U);
    Capture capture;
    router.BindTouchSink(CaptureGuest, &capture);
    router.BindSystemActionSink(CaptureSystem, &capture);

    // Real CST9217 trajectory captured on ESP-Mosaico. On a 480 px panel the
    // proportional recognition distance is 38 px, and this modest diagonal
    // drift must not permanently reject the downward gesture.
    (void)input.Emit(Sample(TouchPhase::kDown, 178U, 22U, 0U));
    (void)input.Emit(Sample(TouchPhase::kMove, 225U, 62U, 137000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 225U, 62U, 160000U));
    return Check(capture.guest_samples.empty(), "recognized 480px top gesture must not leak to Guest") &&
           Check(capture.system_actions.size() == 1U, "480px top gesture must emit exactly one System action") &&
           Check(capture.system_actions[0].type == SystemUiActionType::kOpenStatusLayer,
                 "480px top gesture must open the status layer");
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
    if (!Check(capture.guest_samples.size() == 3U, "bottom swipe outside the reserved 32 rows must reach Guest") ||
        !Check(capture.system_actions.empty(), "bottom swipe outside reserved rows must not suspend")) {
        return false;
    }

    capture = {};
    // A short movement from the physical edge is also insufficient; the
    // deliberate System gesture must travel at least 56 pixels upward.
    (void)input.Emit(Sample(TouchPhase::kDown, 360U, 710U, 200000U));
    (void)input.Emit(Sample(TouchPhase::kMove, 358U, 655U, 290000U));
    (void)input.Emit(Sample(TouchPhase::kUp, 358U, 655U, 310000U));
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
           Check(guest_capture.system_actions.size() == 1U, "replacement track must not emit another System action");
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
    return Check(capture.guest_samples.size() == 3U, "rejected edge drag must replay Down/latest Move/Up") &&
           Check(capture.guest_samples[0].phase == TouchPhase::kDown, "rejected edge drag must begin with Down") &&
           Check(capture.guest_samples[1].phase == TouchPhase::kMove && capture.guest_samples[1].x == 404U &&
                     capture.guest_samples[1].y == 680U,
                 "rejected edge drag must retain its latest pending Move") &&
           Check(capture.guest_samples[2].phase == TouchPhase::kUp, "rejected edge drag must end with Up") &&
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
    const bool passed = NormalTouchPassesThrough() && InjectedTouchTraversesPlatformAndSystemRouter() &&
                        SemanticKeyReachesGuest() && TouchAndKeyInputReportUserActivity() && TopGestureIsReserved() &&
                        TopGestureScalesFor480Display() && BottomGestureIsReserved() &&
                        BottomGestureRejectsIncidentalMovement() &&
                        RecognizedGestureQuarantinesReplacementTrack() &&
                        RejectedEdgeGestureReplaysCompleteSequence() && ReleasedEdgeTapReplaysDownAndUp() &&
                        RejectedEdgeDragRetainsLatestMove() && TimedOutCandidateReturnsToGuest();
    if (!passed) {
        return 1;
    }
    std::puts("SystemGestureRouter host tests passed (13 cases).");
    return 0;
}
