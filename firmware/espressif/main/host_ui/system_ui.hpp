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
constexpr uint8_t kMinimumBrightnessPercent = 0U;
constexpr uint32_t kMaxWifiSsidLength = 32U;
constexpr uint32_t kMaxWifiPasswordLength = 64U;
constexpr uint32_t kMaxSavedWifiNetworks = 8U;
constexpr uint32_t kMaxVisibleWifiNetworks = 16U;

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

struct HallWifiModel final {
    std::array<char, kMaxWifiSsidLength + 1U> ssid{};
    int8_t rssi{};
    bool available{};
    bool enabled{};
    bool connected{};
};

struct HallModel final {
    std::array<HallAppModel, kMaxHallApps> apps{};
    uint32_t app_count{};
    const char* status_app_id{};
    HallStatus status{HallStatus::kReady};
    uint32_t detail{};
    bool launch_enabled{};
    HallWifiModel wifi{};
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
    bool wifi_connecting{};
    bool cellular_available{};
    bool cellular_enabled{};
    bool cellular_connected{};
    bool battery_available{};
    bool performance_overlay_enabled{};
};

enum class SystemMenuItem : uint32_t {
    kWifi,
    kLanguage,
    kSystemInformation,
    kManageApps,
};

struct SystemMenuModel final {
    const char* language{"English"};
    uint32_t installed_app_count{};
    bool wifi_available{};
    bool wifi_enabled{};
    bool wifi_connected{};
    bool wifi_connecting{};
};

constexpr uint32_t kSystemInformationTextCapacity = 64U;

struct MemoryStatisticsModel final {
    uint32_t total_kib{};
    uint32_t free_kib{};
    uint32_t minimum_free_kib{};
    uint32_t largest_free_block_kib{};
};

struct SystemInformationModel final {
    std::array<char, kSystemInformationTextCapacity> firmware_version{};
    std::array<char, kSystemInformationTextCapacity> build_date{};
    std::array<char, kSystemInformationTextCapacity> build_time{};
    std::array<char, kSystemInformationTextCapacity> build_id{};
    std::array<char, kSystemInformationTextCapacity> idf_version{};
    std::array<char, kSystemInformationTextCapacity> host_chip{};
    std::array<char, kSystemInformationTextCapacity> cpu{};
    std::array<char, kSystemInformationTextCapacity> wifi_coprocessor{};
    std::array<char, kSystemInformationTextCapacity> flash_capacity{};
    std::array<char, kSystemInformationTextCapacity> panel{};
    std::array<char, kSystemInformationTextCapacity> display_interface{};
    std::array<char, kSystemInformationTextCapacity> resolution{};
    std::array<char, kSystemInformationTextCapacity> touch_controller{};
    std::array<char, kSystemInformationTextCapacity> uptime{};
    std::array<char, kSystemInformationTextCapacity> last_reset{};
    MemoryStatisticsModel internal_sram{};
    MemoryStatisticsModel psram{};
};

struct ManagedAppModel final {
    const char* app_id{};
    const char* display_name{};
    uint32_t bundle_size_kib{};
};

struct AppManagementModel final {
    std::array<ManagedAppModel, kMaxHallApps> apps{};
    uint32_t app_count{};
    uint32_t storage_used_kib{};
    uint32_t storage_total_kib{};
    bool launch_available{};
    bool uninstall_available{};
};

enum class WifiBand : uint8_t {
    kUnknown,
    k2_4Ghz,
    k5Ghz,
};

enum class WifiConnectionState : uint8_t {
    kDisconnected,
    kConnecting,
    kConnected,
    kAuthenticationFailed,
    kAuthenticationTimedOut,
    kHandshakeTimedOut,
    kNetworkNotFound,
    kFailed,
};

struct WifiNetworkModel final {
    std::array<char, kMaxWifiSsidLength + 1U> ssid{};
    int8_t rssi{};
    uint8_t channel{};
    WifiBand band{WifiBand::kUnknown};
    bool secured{true};
    bool saved{};
    bool connected{};
};

struct WifiSettingsModel final {
    std::array<WifiNetworkModel, kMaxSavedWifiNetworks> saved_networks{};
    std::array<WifiNetworkModel, kMaxVisibleWifiNetworks> available_networks{};
    uint32_t saved_network_count{};
    uint32_t available_network_count{};
    bool available{};
    bool enabled{};
    bool connected{};
    bool scanning{};
    WifiConnectionState connection_state{WifiConnectionState::kDisconnected};
};

enum class SystemUiActionType {
    kLaunchApp,
    kStopApp,
    kSuspendToHall,
    kOpenWifiSettings,
    kOpenSystemMenu,
    kCloseSystemMenu,
    kSelectSystemMenuItem,
    kCloseSystemInformation,
    kCloseAppManagement,
    kLaunchManagedApp,
    kUninstallManagedApp,
    kCloseWifiSettings,
    kOpenWifiNetworkScan,
    kCloseWifiNetworkScan,
    kSetWifiEnabled,
    kConnectSavedWifi,
    kConnectNewWifi,
    kDisconnectWifi,
    kForgetWifi,
    kOpenStatusLayer,
    kCloseStatusLayer,
    kSetBrightness,
    kSetVolume,
    kTogglePerformanceOverlay,
    // Internal Host wake-up signal. SystemUiBackend implementations never emit
    // this action directly.
    kWifiStateChanged,
};

struct SystemUiAction final {
    SystemUiActionType type{SystemUiActionType::kLaunchApp};
    uint32_t app_index{};
    uint32_t value{};
    std::array<char, kMaxWifiSsidLength + 1U> text{};
    std::array<char, kMaxWifiPasswordLength + 1U> secret{};
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
    virtual void UpdateHallWifi(const HallWifiModel& model) = 0;
    virtual void LeaveHall() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> RestoreGuestView() = 0;
    virtual void WatchGuestActions(SystemUiActionSink action_sink, void* action_context) = 0;
    virtual void StopWatchingGuestActions(void* action_context) = 0;
    [[nodiscard]] virtual std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame() = 0;
    virtual void ReleaseGuestSnapshot() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowSystemMenu(const SystemMenuModel& model,
                                                                            SystemUiActionSink action_sink,
                                                                            void* action_context) = 0;
    virtual void UpdateSystemMenu(const SystemMenuModel& model) = 0;
    virtual void LeaveSystemMenu() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowSystemInformation(const SystemInformationModel& model,
                                                                                   SystemUiActionSink action_sink,
                                                                                   void* action_context) = 0;
    virtual void LeaveSystemInformation() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowAppManagement(const AppManagementModel& model,
                                                                               SystemUiActionSink action_sink,
                                                                               void* action_context) = 0;
    virtual void LeaveAppManagement() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowWifiSettings(const WifiSettingsModel& model,
                                                                              SystemUiActionSink action_sink,
                                                                              void* action_context) = 0;
    virtual void UpdateWifiSettings(const WifiSettingsModel& model) = 0;
    virtual void LeaveWifiSettings() = 0;
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
