#include "platform/lvgl/display/scanout_arbiter.hpp"

namespace micropixel::platform::lvgl {

ScanoutArbiter& ScanoutArbiter::Instance() {
    static ScanoutArbiter instance;
    return instance;
}

ScanoutArbiter::ScanoutArbiter() : mutex_(xSemaphoreCreateMutexStatic(&mutex_storage_)) {}

void ScanoutArbiter::RegisterPresenter(const PresenterHooks& hooks) {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    presenter_ = hooks;
    presenter_active_ = false;
    (void)xSemaphoreGive(mutex_);
}

void ScanoutArbiter::UnregisterPresenter(const void* context) {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    if (presenter_.context == context) {
        presenter_ = {};
        presenter_active_ = false;
    }
    (void)xSemaphoreGive(mutex_);
}

void ScanoutArbiter::BeginSystemScanout() {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    ++system_depth_;
    const bool must_yield = presenter_active_ && presenter_.yield != nullptr;
    const PresenterHooks hooks = presenter_;
    (void)xSemaphoreGive(mutex_);
    if (must_yield) {
        // The presenter's yield path calls LeavePresenterScanout(), so the
        // mutex must not be held here.
        hooks.yield(hooks.context);
    }
}

void ScanoutArbiter::EndSystemScanout() {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    if (system_depth_ > 0U) {
        --system_depth_;
    }
    (void)xSemaphoreGive(mutex_);
}

bool ScanoutArbiter::TryEnterPresenterScanout() {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool granted = system_depth_ == 0U && presenter_.context != nullptr;
    if (granted) {
        presenter_active_ = true;
    }
    (void)xSemaphoreGive(mutex_);
    return granted;
}

void ScanoutArbiter::LeavePresenterScanout() {
    (void)xSemaphoreTake(mutex_, portMAX_DELAY);
    presenter_active_ = false;
    (void)xSemaphoreGive(mutex_);
}

}  // namespace micropixel::platform::lvgl
