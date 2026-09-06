#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace micropixel::platform::lvgl {

// Single owner of the panel's dummy-draw scanout. Two kinds of users exist:
//
//   * system compositors (Hall/status transitions) that take the panel for a
//     bounded animation from a Host task, and
//   * the Direct Surface presenter that scans Guest frames out continuously
//     until told otherwise.
//
// The adapter serializes individual dummy-draw calls, but it cannot stop two
// users from interleaving whole frames. This arbiter makes the presenter yield
// synchronously before a system compositor starts and keeps it out until the
// compositor finishes. Presenter (re)entry is a non-blocking attempt so the
// presenter never waits on the LVGL task.
class ScanoutArbiter final {
   public:
    struct PresenterHooks final {
        void* context{};
        // Runs on the caller's task; must return only after the presenter has
        // left dummy draw and will not blit again until TryEnterPresenter succeeds.
        void (*yield)(void* context){};
    };

    static ScanoutArbiter& Instance();

    ScanoutArbiter(const ScanoutArbiter&) = delete;
    ScanoutArbiter& operator=(const ScanoutArbiter&) = delete;

    void RegisterPresenter(const PresenterHooks& hooks);
    void UnregisterPresenter(const void* context);

    // System compositor side. Begin blocks until the presenter yielded; nested
    // Begin/End pairs from the same owner are counted.
    void BeginSystemScanout();
    void EndSystemScanout();

    // Presenter side.
    [[nodiscard]] bool TryEnterPresenterScanout();
    void LeavePresenterScanout();

   private:
    ScanoutArbiter();

    StaticSemaphore_t mutex_storage_{};
    SemaphoreHandle_t mutex_{};
    PresenterHooks presenter_{};
    unsigned system_depth_{};
    bool presenter_active_{};
};

// Scoped BeginSystemScanout/EndSystemScanout for synchronous system
// compositors (status layer, Hall transitions).
class SystemScanoutScope final {
   public:
    SystemScanoutScope() { ScanoutArbiter::Instance().BeginSystemScanout(); }
    ~SystemScanoutScope() { ScanoutArbiter::Instance().EndSystemScanout(); }
    SystemScanoutScope(const SystemScanoutScope&) = delete;
    SystemScanoutScope& operator=(const SystemScanoutScope&) = delete;
};

}  // namespace micropixel::platform::lvgl
