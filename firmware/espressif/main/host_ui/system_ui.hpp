#ifndef MICROPIXEL_HOST_UI_SYSTEM_UI_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_UI_HPP

#include <array>
#include <cstdint>
#include <expected>

namespace micropixel::host_ui {

enum class HallStatus {
    kReady,
    kNoApps,
    kAppExited,
    kAppFailed,
    kRuntimeUnavailable,
    kHostFailure,
};

constexpr uint32_t kMaxHallApps = 3U;
constexpr uint8_t kMinimumBrightnessPercent = 2U;

enum class HallCoverFormat : uint8_t {
    kRgb888,
    kPng,
};

struct HallCoverModel final {
    const uint8_t* data{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    HallCoverFormat format{HallCoverFormat::kRgb888};
    // Stable across mmap lifetimes. A changed key invalidates the platform's
    // PSRAM thumbnail cache for this Hall slot.
    uint64_t cache_key{};
};

struct HallAppModel final {
    const char* app_id{};
    const char* display_name{};
    HallCoverModel cover{};
    bool running{};
};

struct HallModel final {
    std::array<HallAppModel, kMaxHallApps> apps{};
    uint32_t app_count{};
    const char* status_app_id{};
    HallStatus status{HallStatus::kReady};
    uint32_t detail{};
    bool launch_enabled{};
};

struct StatusLayerModel final {
    uint32_t memory_used_kib{};
    uint32_t memory_total_kib{};
    uint32_t storage_used_kib{};
    uint32_t storage_total_kib{};
    uint8_t battery_percent{};
    uint8_t brightness_percent{80U};
    uint8_t volume_percent{70U};
    bool wifi_available{};
    bool wifi_enabled{};
    bool wifi_connected{};
    bool cellular_available{};
    bool cellular_enabled{};
    bool cellular_connected{};
    bool battery_available{};
    bool performance_overlay_enabled{};
};

enum class SystemUiActionType {
    kLaunchApp,
    kStopApp,
    kSuspendToHall,
    kOpenStatusLayer,
    kCloseStatusLayer,
    kSetBrightness,
    kSetVolume,
    kTogglePerformanceOverlay,
};

struct SystemUiAction final {
    SystemUiActionType type{SystemUiActionType::kLaunchApp};
    uint32_t app_index{};
    uint32_t value{};
};

using SystemUiActionSink = void (*)(void* context, const SystemUiAction& action);

enum class SystemUiError {
    kUnavailable,
    kRenderFailed,
};

class SystemUiBackend {
   public:
    virtual ~SystemUiBackend() = default;
    SystemUiBackend(const SystemUiBackend&) = delete;
    SystemUiBackend& operator=(const SystemUiBackend&) = delete;

    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowHall(const HallModel& model,
                                                                      SystemUiActionSink action_sink,
                                                                      void* action_context) = 0;
    virtual void LeaveHall() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> RestoreGuestView() = 0;
    virtual void WatchGuestActions(SystemUiActionSink action_sink, void* action_context) = 0;
    virtual void StopWatchingGuestActions(void* action_context) = 0;
    [[nodiscard]] virtual std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame() = 0;
    virtual void ReleaseGuestSnapshot() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowStatusLayer(const StatusLayerModel& model,
                                                                             SystemUiActionSink action_sink,
                                                                             void* action_context) = 0;
    virtual void UpdateStatusLayer(const StatusLayerModel& model) = 0;
    virtual void LeaveStatusLayer() = 0;
    virtual void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) = 0;
    virtual void ApplyBrightness(uint8_t percent) = 0;
    virtual void ApplyVolume(uint8_t percent) = 0;

   protected:
    SystemUiBackend() = default;
};

}  // namespace micropixel::host_ui

#endif
