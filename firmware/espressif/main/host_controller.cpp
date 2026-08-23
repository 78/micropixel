#include "host_controller.hpp"

#include <array>
#include <cinttypes>
#include <limits>
#include <optional>
#include <utility>

#include "app_controller.hpp"
#include "device/device_services.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "host_ui/system_settings_store.hpp"
#include "host_ui/system_shell.hpp"
#include "runtime/app_runtime.hpp"
#include "runtime/wamr/diagnostics.h"
#include "task_policy.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_host";
constexpr TickType_t kCooperativeStopTimeout = pdMS_TO_TICKS(500);
constexpr TickType_t kForcedStopTimeout = pdMS_TO_TICKS(2500);
constexpr int64_t kPerformanceSamplePeriodUs = 500000;

class CpuUsageSampler final {
   public:
    void Reset() {
        last_sample_us_ = 0U;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        last_idle_time_ = 0U;
#endif
    }

    [[nodiscard]] uint8_t Sample() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        using Counter = configRUN_TIME_COUNTER_TYPE;
        Counter idle_time = 0U;
#if defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP
        idle_time = ulTaskGetIdleRunTimeCounter();
#else
        for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
            idle_time += ulTaskGetIdleRunTimeCounterForCore(core);
        }
#endif
        const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
        if (last_sample_us_ == 0U) {
            last_sample_us_ = now_us;
            last_idle_time_ = idle_time;
            return 0U;
        }
        const uint64_t elapsed_us = now_us - last_sample_us_;
        const Counter idle_delta = idle_time >= last_idle_time_
                                       ? idle_time - last_idle_time_
                                       : std::numeric_limits<Counter>::max() - last_idle_time_ + idle_time + 1U;
        last_sample_us_ = now_us;
        last_idle_time_ = idle_time;
        const uint64_t available_cpu_us = elapsed_us * configNUMBER_OF_CORES;
        if (available_cpu_us == 0U) {
            return 0U;
        }
        const uint64_t idle_percent = static_cast<uint64_t>(idle_delta) * 100U / available_cpu_us;
        return static_cast<uint8_t>(idle_percent < 100U ? 100U - idle_percent : 0U);
#else
        return 0U;
#endif
    }

   private:
    uint64_t last_sample_us_{};
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    configRUN_TIME_COUNTER_TYPE last_idle_time_{};
#endif
};

using HallCoverMappings = std::array<runtime::LaunchAssetMapping, runtime::kMaxInstalledApps>;

host_ui::HallModel MakeHallModel(const runtime::InstalledAppCatalog& catalog, host_ui::HallStatus status,
                                 const runtime::AppRunOutcome* outcome = nullptr, uint32_t detail = 0U,
                                 bool launch_enabled = true, const HallCoverMappings* covers = nullptr,
                                 const std::optional<uint32_t>& suspended_index = std::nullopt,
                                 const host_ui::HallCoverModel* suspended_snapshot = nullptr) {
    host_ui::HallModel model{.app_count = catalog.count,
                             .status_app_id = outcome != nullptr ? outcome->app_id.data() : nullptr,
                             .status = status,
                             .detail = detail,
                             .launch_enabled = launch_enabled};
    for (uint32_t index = 0U; index < catalog.count && index < host_ui::kMaxHallApps; ++index) {
        model.apps[index].app_id = catalog.apps[index].app_id.data();
        model.apps[index].display_name = catalog.apps[index].display_name.data();
        if (covers != nullptr && (*covers)[index].valid()) {
            const auto& asset = (*covers)[index].asset();
            model.apps[index].cover = host_ui::HallCoverModel{
                .data = asset.data,
                .size = asset.size,
                .width = asset.width,
                .height = asset.height,
                .stride = asset.stride,
                .format = asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG ? host_ui::HallCoverFormat::kPng
                                                                       : host_ui::HallCoverFormat::kRgb888,
                .cache_key = (static_cast<uint64_t>(catalog.apps[index].store_offset) << 32U) | asset.content_hash};
        }
        if (suspended_index.has_value() && *suspended_index == index) {
            model.apps[index].running = true;
            if (suspended_snapshot != nullptr && suspended_snapshot->data != nullptr) {
                model.apps[index].cover = *suspended_snapshot;
            }
        }
    }
    return model;
}

HallCoverMappings OpenHallCovers(const runtime::InstalledAppCatalog& catalog,
                                 const std::optional<uint32_t>& snapshot_index = std::nullopt) {
    HallCoverMappings covers{};
    for (uint32_t index = 0U; index < catalog.count && index < static_cast<uint32_t>(covers.size()); ++index) {
        if (snapshot_index.has_value() && *snapshot_index == index) {
            continue;
        }
        auto mapping_result = runtime::LaunchAssetMapping::Open(catalog.apps[index].store_offset);
        if (!mapping_result) {
            ESP_LOGW(kTag, "App Hall cover unavailable: index=%" PRIu32 " app=%s", index,
                     catalog.apps[index].app_id.data());
            continue;
        }
        covers[index] = std::move(*mapping_result);
    }
    return covers;
}

std::expected<runtime::AppRunOutcome, AppControllerError> StopApp(AppController& controller) {
    return controller.Stop(kCooperativeStopTimeout, kForcedStopTimeout);
}

bool ShowHall(host_ui::SystemShell& shell, const host_ui::HallModel& model) {
    auto result = shell.ShowHall(model);
    if (!result) {
        ESP_LOGE(kTag, "failed to render App Hall: error=%u", static_cast<unsigned>(result.error()));
        return false;
    }
    return true;
}

void RefreshStatusMetrics(host_ui::StatusLayerModel& model, const runtime::InstalledAppCatalog& catalog) {
    const size_t memory_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t memory_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    model.memory_total_kib = static_cast<uint32_t>(memory_total / 1024U);
    model.memory_used_kib = static_cast<uint32_t>((memory_total - memory_free) / 1024U);

    const esp_partition_t* app_store =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "app_store");
    model.storage_total_kib = app_store != nullptr ? app_store->size / 1024U : 0U;
    uint32_t used_bytes = 0U;
    for (uint32_t index = 0U; index < catalog.count; ++index) {
        const runtime::InstalledApp& app = catalog.apps[index];
        const uint32_t app_end = app.store_offset + app.bundle_size;
        used_bytes = app_end > used_bytes ? app_end : used_bytes;
    }
    model.storage_used_kib = used_bytes / 1024U;
}

bool RunStatusLayer(host_ui::SystemShell& shell, AppController* controller, host_ui::StatusLayerModel& model,
                    const runtime::InstalledAppCatalog& catalog, host_ui::SystemSettingsStore& settings_store) {
    if (controller != nullptr) {
        auto suspend_result = controller->Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for status layer: error=%u",
                     static_cast<unsigned>(suspend_result.error()));
            return false;
        }
        shell.StopWatchingGuestActions();
    }
    RefreshStatusMetrics(model, catalog);
    auto show_result = shell.ShowStatusLayer(model);
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show status layer: error=%u", static_cast<unsigned>(show_result.error()));
        if (controller == nullptr) {
            return false;
        }
        auto resume_result = controller->Resume();
        if (resume_result) {
            shell.WatchGuestActions();
        }
        return resume_result.has_value();
    }
    if (model.performance_overlay_enabled) {
        shell.UpdatePerformanceOverlay(false, 0U);
        shell.UpdatePerformanceOverlay(true, 0U);
    }

    CpuUsageSampler paused_cpu_sampler;
    paused_cpu_sampler.Reset();
    (void)paused_cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    bool close = false;
    bool settings_changed = false;
    while (!close) {
        const TickType_t timeout = model.performance_overlay_enabled ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        const auto action = shell.PollAction(timeout);
        if (!action.has_value()) {
            const int64_t now_us = esp_timer_get_time();
            if (model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell.UpdatePerformanceOverlay(true, paused_cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            continue;
        }
        bool controls_changed = false;
        switch (action->type) {
            case host_ui::SystemUiActionType::kCloseStatusLayer:
            case host_ui::SystemUiActionType::kSuspendToHall:
                close = true;
                break;
            case host_ui::SystemUiActionType::kSetBrightness: {
                const uint8_t brightness_percent =
                    static_cast<uint8_t>(action->value < host_ui::kMinimumBrightnessPercent
                                             ? host_ui::kMinimumBrightnessPercent
                                             : (action->value <= 100U ? action->value : 100U));
                settings_changed = settings_changed || model.brightness_percent != brightness_percent;
                model.brightness_percent = brightness_percent;
                shell.ApplyBrightness(model.brightness_percent);
                break;
            }
            case host_ui::SystemUiActionType::kSetVolume: {
                const uint8_t volume_percent = static_cast<uint8_t>(action->value <= 100U ? action->value : 100U);
                settings_changed = settings_changed || model.volume_percent != volume_percent;
                model.volume_percent = volume_percent;
                shell.ApplyVolume(model.volume_percent);
                break;
            }
            case host_ui::SystemUiActionType::kTogglePerformanceOverlay:
                model.performance_overlay_enabled = !model.performance_overlay_enabled;
                settings_changed = true;
                if (model.performance_overlay_enabled) {
                    paused_cpu_sampler.Reset();
                    (void)paused_cpu_sampler.Sample();
                    next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                    shell.UpdatePerformanceOverlay(true, 0U);
                } else {
                    shell.UpdatePerformanceOverlay(false, 0U);
                }
                controls_changed = true;
                break;
            case host_ui::SystemUiActionType::kOpenStatusLayer:
                break;
            default:
                ESP_LOGW(kTag, "ignored action=%u while status layer is visible", static_cast<unsigned>(action->type));
                break;
        }
        if (!close && controls_changed) {
            shell.UpdateStatusLayer(model);
        }
        const int64_t now_us = esp_timer_get_time();
        if (!close && model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
            shell.UpdatePerformanceOverlay(true, paused_cpu_sampler.Sample());
            next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
        }
    }

    if (settings_changed && settings_store.ready() && !settings_store.Save(model)) {
        ESP_LOGW(kTag, "Host settings changed but could not be persisted");
    }
    shell.LeaveStatusLayer();
    if (controller == nullptr) {
        return true;
    }
    auto resume_result = controller->Resume();
    if (!resume_result) {
        ESP_LOGE(kTag, "failed to resume App after status layer: error=%u",
                 static_cast<unsigned>(resume_result.error()));
        (void)controller->RequestStop();
        return false;
    }
    shell.WatchGuestActions();
    return true;
}

void RunUnavailableHall(host_ui::SystemShell& shell, const runtime::InstalledAppCatalog& catalog,
                        host_ui::HallStatus status, uint32_t detail, host_ui::StatusLayerModel& status_model,
                        host_ui::SystemSettingsStore& settings_store) {
    HallCoverMappings covers = OpenHallCovers(catalog);
    CpuUsageSampler cpu_sampler;
    cpu_sampler.Reset();
    (void)cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    for (;;) {
        if (!ShowHall(shell, MakeHallModel(catalog, status, nullptr, detail, false, &covers))) {
            return;
        }
        shell.UpdatePerformanceOverlay(status_model.performance_overlay_enabled, 0U);
        for (;;) {
            const TickType_t timeout = status_model.performance_overlay_enabled ? pdMS_TO_TICKS(20) : portMAX_DELAY;
            const auto action = shell.PollAction(timeout);
            const int64_t now_us = esp_timer_get_time();
            if (status_model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (!action.has_value()) {
                continue;
            }
            if (action->type != host_ui::SystemUiActionType::kOpenStatusLayer) {
                ESP_LOGW(kTag, "ignored action=%u while App launch is unavailable",
                         static_cast<unsigned>(action->type));
                continue;
            }
            (void)RunStatusLayer(shell, nullptr, status_model, catalog, settings_store);
            cpu_sampler.Reset();
            (void)cpu_sampler.Sample();
            next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
            break;
        }
    }
}

class ActiveHost final {
   public:
    ActiveHost(runtime::InstalledAppCatalog catalog, runtime::AppRuntime& runtime, host_ui::SystemShell& shell,
               host_ui::StatusLayerModel& status_model, host_ui::SystemSettingsStore& settings_store)
        : catalog_(std::move(catalog)),
          app_controller_(runtime),
          shell_(shell),
          status_model_(status_model),
          settings_store_(settings_store),
          hall_status_(catalog_.count == 0U ? host_ui::HallStatus::kNoApps : host_ui::HallStatus::kReady) {}

    [[nodiscard]] bool Valid() const { return app_controller_.valid(); }

    void Run() {
        for (;;) {
            switch (state_) {
                case State::kHall:
                    if (!RunHall()) {
                        return;
                    }
                    break;
                case State::kForeground:
                    RunForeground();
                    break;
            }
        }
    }

   private:
    enum class State { kHall, kForeground };

    [[nodiscard]] bool CanLaunch() const {
        const AppLifecycleState lifecycle = app_controller_.state();
        return catalog_.count != 0U &&
               (lifecycle == AppLifecycleState::kNotRunning || lifecycle == AppLifecycleState::kSuspended);
    }

    [[nodiscard]] bool ShowCurrentHall(const HallCoverMappings& covers) {
        if (!ShowHall(shell_, MakeHallModel(catalog_, hall_status_, outcome_, hall_detail_, CanLaunch(), &covers,
                                            suspended_index_, &suspended_snapshot_))) {
            return false;
        }
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        return true;
    }

    void RecordHostFailure(uint32_t detail) {
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kHostFailure;
        hall_detail_ = detail;
    }

    [[nodiscard]] bool RunHall() {
        const std::optional<uint32_t> snapshot_index =
            suspended_snapshot_.data != nullptr ? suspended_index_ : std::nullopt;
        HallCoverMappings covers = OpenHallCovers(catalog_, snapshot_index);
        if (!ShowCurrentHall(covers)) {
            return false;
        }

        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        for (;;) {
            const TickType_t timeout = status_model_.performance_overlay_enabled ? pdMS_TO_TICKS(20) : portMAX_DELAY;
            const auto pending_action = shell_.PollAction(timeout);
            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (!pending_action.has_value()) {
                continue;
            }

            const host_ui::SystemUiAction action = *pending_action;
            if (action.type == host_ui::SystemUiActionType::kLaunchApp && action.app_index < catalog_.count &&
                CanLaunch()) {
                ActivateSelectedApp(action.app_index);
                return true;
            }
            if (action.type == host_ui::SystemUiActionType::kStopApp && suspended_index_.has_value() &&
                action.app_index == *suspended_index_) {
                if (!StopSuspendedApp(covers)) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                if (!RunStatusLayer(shell_, nullptr, status_model_, catalog_, settings_store_)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (!ShowCurrentHall(covers)) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            ESP_LOGW(kTag, "ignored invalid App Hall action: type=%u index=%" PRIu32,
                     static_cast<unsigned>(action.type), action.app_index);
        }
    }

    [[nodiscard]] bool StopSuspendedApp(HallCoverMappings& covers) {
        const uint32_t stopped_index = *suspended_index_;
        ESP_LOGI(kTag, "stopping suspended App from Hall: index=%" PRIu32, stopped_index);
        auto stopped_result = StopApp(app_controller_);
        if (!stopped_result) {
            ESP_LOGE(kTag, "Hall could not complete suspended App stop: error=%u",
                     static_cast<unsigned>(stopped_result.error()));
            RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
            return ShowCurrentHall(covers);
        }

        suspended_index_.reset();
        auto stopped_cover = runtime::LaunchAssetMapping::Open(catalog_.apps[stopped_index].store_offset);
        if (stopped_cover) {
            covers[stopped_index] = std::move(*stopped_cover);
        } else {
            ESP_LOGW(kTag, "stopped App cover unavailable: index=%" PRIu32 " app=%s", stopped_index,
                     catalog_.apps[stopped_index].app_id.data());
        }
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        if (!ShowCurrentHall(covers)) {
            return false;
        }
        shell_.ReleaseGuestSnapshot();
        suspended_snapshot_ = {};
        micropixel_log_heap_state("host after Hall App stop");
        return true;
    }

    void ActivateSelectedApp(uint32_t selected_index) {
        bool resumed_existing = false;
        if (suspended_index_.has_value()) {
            const bool selected_suspended_app = *suspended_index_ == selected_index;
            bool guest_view_restored = true;
            if (selected_suspended_app) {
                auto restore_result = shell_.RestoreGuestView();
                if (!restore_result) {
                    ESP_LOGE(kTag, "failed to restore retained Guest view: error=%u",
                             static_cast<unsigned>(restore_result.error()));
                    shell_.LeaveHall();
                    guest_view_restored = false;
                }
            } else {
                shell_.LeaveHall();
            }
            shell_.ReleaseGuestSnapshot();
            suspended_snapshot_ = {};
            suspended_index_.reset();

            if (selected_suspended_app) {
                if (!guest_view_restored) {
                    (void)StopApp(app_controller_);
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                    return;
                }
                auto resume_result = app_controller_.Resume();
                if (!resume_result) {
                    ESP_LOGE(kTag, "failed to resume suspended App: error=%u",
                             static_cast<unsigned>(resume_result.error()));
                    auto stopped_result = StopApp(app_controller_);
                    RecordHostFailure(static_cast<uint32_t>(resume_result.error()));
                    if (!stopped_result) {
                        hall_detail_ = static_cast<uint32_t>(stopped_result.error());
                    }
                    return;
                }
                ESP_LOGI(kTag, "resumed selected App: index=%" PRIu32, selected_index);
                resumed_existing = true;
            } else {
                ESP_LOGI(kTag, "switching App: stopping suspended index before launching index=%" PRIu32,
                         selected_index);
                auto stopped_result = StopApp(app_controller_);
                if (!stopped_result) {
                    RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
                    return;
                }
                micropixel_log_heap_state("host after suspended App stop");
            }
        } else {
            shell_.LeaveHall();
        }

        const runtime::InstalledApp& selected_app = catalog_.apps[selected_index];
        if (!resumed_existing) {
            ESP_LOGI(kTag, "launching selected App: index=%" PRIu32 " app=%s offset=0x%" PRIx32, selected_index,
                     selected_app.app_id.data(), selected_app.store_offset);
            micropixel_log_heap_state("host before AppController start");
            auto start_result = app_controller_.Start(selected_app);
            if (!start_result) {
                ESP_LOGE(kTag, "Host could not start the selected AppSession: error=%u",
                         static_cast<unsigned>(start_result.error()));
                RecordHostFailure(static_cast<uint32_t>(start_result.error()));
                return;
            }
        }

        foreground_index_ = selected_index;
        state_ = State::kForeground;
    }

    void RunForeground() {
        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        shell_.WatchGuestActions();

        for (;;) {
            auto completion_result = app_controller_.PollCompletion(pdMS_TO_TICKS(20));
            if (!completion_result) {
                ESP_LOGE(kTag, "Host could not join the completed AppSession: error=%u",
                         static_cast<unsigned>(completion_result.error()));
                LeaveForegroundUi();
                RecordHostFailure(static_cast<uint32_t>(completion_result.error()));
                state_ = State::kHall;
                return;
            }
            if (completion_result->has_value()) {
                last_outcome_ = **completion_result;
                LeaveForegroundUi();
                outcome_ = &last_outcome_;
                hall_status_ = last_outcome_.completion == runtime::AppCompletion::kFailed
                                   ? host_ui::HallStatus::kAppFailed
                                   : host_ui::HallStatus::kAppExited;
                hall_detail_ = last_outcome_.completion == runtime::AppCompletion::kFailed
                                   ? static_cast<uint32_t>(last_outcome_.error)
                                   : 0U;
                if (last_outcome_.completion == runtime::AppCompletion::kFailed) {
                    ESP_LOGE(kTag, "AppSession failed: app=%s error=%u", last_outcome_.app_id.data(),
                             static_cast<unsigned>(last_outcome_.error));
                }
                micropixel_log_heap_state("host after AppController completion");
                state_ = State::kHall;
                return;
            }

            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }

            const auto action = shell_.PollAction(0U);
            if (!action.has_value()) {
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                OpenForegroundStatusLayer(cpu_sampler, next_performance_sample_us);
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kSuspendToHall &&
                app_controller_.state() == AppLifecycleState::kForeground) {
                SuspendToHall();
                return;
            }
            ESP_LOGW(kTag, "ignored Guest system action=%u in lifecycle state=%u", static_cast<unsigned>(action->type),
                     static_cast<unsigned>(app_controller_.state()));
        }
    }

    void OpenForegroundStatusLayer(CpuUsageSampler& cpu_sampler, int64_t& next_performance_sample_us) {
        if (app_controller_.state() != AppLifecycleState::kForeground) {
            ESP_LOGW(kTag, "ignored status-layer gesture in lifecycle state=%u",
                     static_cast<unsigned>(app_controller_.state()));
            return;
        }
        if (!RunStatusLayer(shell_, &app_controller_, status_model_, catalog_, settings_store_)) {
            RecordHostFailure(static_cast<uint32_t>(AppControllerError::kResumeFailed));
        }
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        shell_.UpdatePerformanceOverlay(false, 0U);
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    }

    void SuspendToHall() {
        auto suspend_result = app_controller_.Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for Hall: error=%u", static_cast<unsigned>(suspend_result.error()));
            RecordHostFailure(static_cast<uint32_t>(suspend_result.error()));
            return;
        }
        shell_.StopWatchingGuestActions();
        auto snapshot_result = shell_.CaptureGuestFrame();
        if (snapshot_result) {
            suspended_snapshot_ = *snapshot_result;
        } else {
            suspended_snapshot_ = {};
            ESP_LOGW(kTag, "running-card snapshot unavailable: error=%u; using Flash cover",
                     static_cast<unsigned>(snapshot_result.error()));
        }
        suspended_index_ = foreground_index_;
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        LeaveForegroundUi();
        ESP_LOGI(kTag, "suspended App moved to Hall: index=%" PRIu32, foreground_index_);
        state_ = State::kHall;
    }

    void LeaveForegroundUi() {
        shell_.StopWatchingGuestActions();
        shell_.UpdatePerformanceOverlay(false, 0U);
    }

    runtime::InstalledAppCatalog catalog_;
    AppController app_controller_;
    host_ui::SystemShell& shell_;
    host_ui::StatusLayerModel& status_model_;
    host_ui::SystemSettingsStore& settings_store_;
    State state_{State::kHall};
    runtime::AppRunOutcome last_outcome_{};
    const runtime::AppRunOutcome* outcome_{};
    host_ui::HallStatus hall_status_;
    uint32_t hall_detail_{};
    uint32_t foreground_index_{};
    std::optional<uint32_t> suspended_index_;
    host_ui::HallCoverModel suspended_snapshot_{};
};

}  // namespace

HostController::HostController(device::DeviceServices& devices, host_ui::SystemShell& shell)
    : devices_(devices), shell_(shell) {}

void HostController::Run() {
    vTaskPrioritySet(nullptr, task_policy::kHostPriority);
    ESP_LOGI(kTag, "Host supervisor task priority=%u", static_cast<unsigned>(uxTaskPriorityGet(nullptr)));

    host_ui::SystemSettingsStore settings_store;
    if (!settings_store.Initialize()) {
        ESP_LOGW(kTag, "Host settings persistence is unavailable; using defaults for this boot");
    }
    host_ui::StatusLayerModel status_model{};
    if (settings_store.ready() && !settings_store.Load(status_model)) {
        ESP_LOGW(kTag, "Host settings could not be restored; using safe defaults");
    }
    shell_.ApplyBrightness(status_model.brightness_percent);
    shell_.ApplyVolume(status_model.volume_percent);

    auto catalog_result = runtime::ScanInstalledApps();
    if (!catalog_result) {
        ESP_LOGE(kTag, "App Store catalog scan failed");
        const runtime::InstalledAppCatalog empty_catalog{};
        RunUnavailableHall(shell_, empty_catalog, host_ui::HallStatus::kNoApps,
                           static_cast<uint32_t>(catalog_result.error()), status_model, settings_store);
        return;
    }
    runtime::InstalledAppCatalog catalog = *catalog_result;

    auto runtime_result = runtime::AppRuntime::Initialize(devices_);
    if (!runtime_result) {
        ESP_LOGE(kTag, "AppRuntime initialization failed: error=%u", static_cast<unsigned>(runtime_result.error()));
        RunUnavailableHall(shell_, catalog, host_ui::HallStatus::kRuntimeUnavailable,
                           static_cast<uint32_t>(runtime_result.error()), status_model, settings_store);
        return;
    }

    runtime::AppRuntime app_runtime = std::move(*runtime_result);
    ActiveHost active_host(catalog, app_runtime, shell_, status_model, settings_store);
    if (!active_host.Valid()) {
        ESP_LOGE(kTag, "AppController initialization failed");
        RunUnavailableHall(shell_, catalog, host_ui::HallStatus::kRuntimeUnavailable,
                           static_cast<uint32_t>(AppControllerError::kUnavailable), status_model, settings_store);
        return;
    }
    active_host.Run();
}

}  // namespace micropixel::firmware
