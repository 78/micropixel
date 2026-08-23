#include "firmware_app.hpp"

#include <pthread.h>

#include <cinttypes>
#include <cstddef>

#include "device/device_services.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "platform/platform.hpp"
#include "runtime/engine.hpp"
#include "runtime/wamr/diagnostics.h"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_main";
constexpr size_t kWamrTaskStackSize = 16U * 1024U;
constexpr int kWamrTaskCore = 0;
constexpr uint32_t kGuestRestartDelayMs = 1000U;

class PthreadAttributes final {
   public:
    PthreadAttributes() : initialized_(pthread_attr_init(&attributes_) == 0) {}
    PthreadAttributes(const PthreadAttributes&) = delete;
    PthreadAttributes& operator=(const PthreadAttributes&) = delete;

    ~PthreadAttributes() {
        if (initialized_) {
            (void)pthread_attr_destroy(&attributes_);
        }
    }

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return initialized_;
    }

    [[nodiscard]] bool Configure(size_t stack_size) {
        return initialized_ && pthread_attr_setdetachstate(&attributes_, PTHREAD_CREATE_JOINABLE) == 0 &&
               pthread_attr_setstacksize(&attributes_, stack_size) == 0;
    }

    [[nodiscard]] const pthread_attr_t* native_handle() const {  // NOLINT(readability-identifier-naming)
        return &attributes_;
    }

   private:
    pthread_attr_t attributes_{};
    bool initialized_{};
};

class ScopedPthreadConfiguration final {
   public:
    ScopedPthreadConfiguration() {
        parent_config_found_ = esp_pthread_get_cfg(&parent_config_) == ESP_OK;
        esp_pthread_cfg_t wamr_config = parent_config_found_ ? parent_config_ : esp_pthread_get_default_config();
        wamr_config.pin_to_core = kWamrTaskCore;
        wamr_config.inherit_cfg = false;
        priority_ = wamr_config.prio;
        applied_ = esp_pthread_set_cfg(&wamr_config) == ESP_OK;
    }

    ScopedPthreadConfiguration(const ScopedPthreadConfiguration&) = delete;
    ScopedPthreadConfiguration& operator=(const ScopedPthreadConfiguration&) = delete;

    ~ScopedPthreadConfiguration() { Restore(); }

    [[nodiscard]] bool applied() const {  // NOLINT(readability-identifier-naming)
        return applied_;
    }
    [[nodiscard]] int priority() const {  // NOLINT(readability-identifier-naming)
        return priority_;
    }

    void Restore() {
        if (!applied_) {
            return;
        }
        if (parent_config_found_) {
            (void)esp_pthread_set_cfg(&parent_config_);
        } else {
            esp_pthread_cfg_t default_config = esp_pthread_get_default_config();
            (void)esp_pthread_set_cfg(&default_config);
        }
        applied_ = false;
    }

   private:
    esp_pthread_cfg_t parent_config_{};
    int priority_{};
    bool parent_config_found_{};
    bool applied_{};
};

class JoinableThread final {
   public:
    JoinableThread() = default;
    JoinableThread(const JoinableThread&) = delete;
    JoinableThread& operator=(const JoinableThread&) = delete;

    ~JoinableThread() {
        if (joinable_) {
            (void)pthread_detach(thread_);
        }
    }

    [[nodiscard]] bool Start(const pthread_attr_t* attributes, void* (*entry)(void*), void* argument) {
        if (joinable_ || pthread_create(&thread_, attributes, entry, argument) != 0) {
            return false;
        }
        joinable_ = true;
        return true;
    }

    [[nodiscard]] bool Join() {
        if (!joinable_ || pthread_join(thread_, nullptr) != 0) {
            return false;
        }
        joinable_ = false;
        return true;
    }

   private:
    pthread_t thread_{};
    bool joinable_{};
};

void* RunEngine(void* argument) {
    auto& devices = *static_cast<device::DeviceServices*>(argument);
    runtime::Engine engine(devices);
    auto result = engine.Run();
    if (!result) {
        ESP_LOGE(kTag, "Guest runtime session failed with error=%u", static_cast<unsigned>(result.error()));
    }
    return nullptr;
}

}  // namespace

void FirmwareApp::Run() {
    if (!InitializePlatform()) {
        return;
    }
    if (!RunGuest()) {
        return;
    }
    RestartAfterGuest();
}

std::expected<void, FirmwareApp::StartupError> FirmwareApp::InitializePlatform() {
    esp_err_t nvs_error = nvs_flash_init_partition("runtime_nvs");
    if (nvs_error != ESP_OK) {
        ESP_LOGE(kTag, "runtime_nvs initialization failed without destructive recovery: %s",
                 esp_err_to_name(nvs_error));
        return std::unexpected(StartupError::kNvsInitialization);
    }
    ESP_LOGI(kTag, "runtime_nvs initialized; Guest data preserved across Host OTA/restart");

    if (platform_.Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "configured platform did not initialize");
        return std::unexpected(StartupError::kPlatformInitialization);
    }
    return {};
}

std::expected<void, FirmwareApp::StartupError> FirmwareApp::RunGuest() {
    micropixel_log_heap_state("app_main before WAMR task");

    ScopedPthreadConfiguration configuration;
    if (!configuration.applied()) {
        ESP_LOGE(kTag, "failed to configure WAMR task affinity");
        return std::unexpected(StartupError::kPthreadConfiguration);
    }
    ESP_LOGI(kTag, "WAMR task policy: core=%d priority=%d (FPU-stable affinity)", kWamrTaskCore,
             configuration.priority());

    PthreadAttributes attributes;
    if (!attributes.valid() || !attributes.Configure(kWamrTaskStackSize)) {
        ESP_LOGE(kTag, "invalid WAMR pthread attributes or stack size");
        return std::unexpected(StartupError::kPthreadAttributes);
    }

    JoinableThread thread;
    device::DeviceServices devices(platform_.graphics(), platform_.input(), platform_.audio(), platform_.random());
    if (!thread.Start(attributes.native_handle(), RunEngine, &devices)) {
        ESP_LOGE(kTag, "failed to create WAMR pthread");
        return std::unexpected(StartupError::kThreadCreation);
    }

    // Restore the app_main pthread policy as soon as the child has inherited
    // its dedicated configuration, matching the ESP-IDF entry-point contract.
    configuration.Restore();
    if (!thread.Join()) {
        ESP_LOGE(kTag, "failed to join WAMR pthread");
        return std::unexpected(StartupError::kThreadJoin);
    }

    micropixel_log_heap_state("app_main after WAMR task join");
    return {};
}

[[noreturn]] void FirmwareApp::RestartAfterGuest() {
    ESP_LOGW(kTag, "Guest terminated; restarting device in %" PRIu32 " ms", kGuestRestartDelayMs);
    vTaskDelay(pdMS_TO_TICKS(kGuestRestartDelayMs));
    esp_restart();
}

}  // namespace micropixel::firmware
