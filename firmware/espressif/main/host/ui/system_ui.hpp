#ifndef MICROPIXEL_HOST_UI_SYSTEM_UI_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_UI_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace micropixel::host_ui {

enum class HallStatus {
    kReady,
    kNoApps,
    kAppExited,
    kAppFailed,
    kRuntimeUnavailable,
    kHostFailure,
};

constexpr uint32_t kMaxHallApps = 50U;
constexpr uint8_t kMinimumBrightnessPercent = 0U;
constexpr uint8_t kDefaultAutoSleepTimeoutMinutes = 5U;
constexpr uint32_t kMaxWifiSsidLength = 32U;
constexpr uint32_t kMinWifiPasswordLength = 8U;
constexpr uint32_t kMaxWifiPasswordLength = 64U;
constexpr uint32_t kMaxSavedWifiNetworks = 8U;
constexpr uint32_t kMaxVisibleWifiNetworks = 16U;
constexpr uint32_t kFirmwareVersionTextCapacity = 64U;
constexpr uint32_t kFirmwareUpdateMessageCapacity = 96U;

enum class FirmwareUpdateState : uint8_t {
    kUnknown,
    kChecking,
    kCurrent,
    kAvailable,
    kDownloading,
    kVerifying,
    kInstalling,
    kFailed,
};

enum class HallCoverFormat : uint8_t {
    kRgb888,
    kJpeg,
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

using ScreenCaptureRelease = void (*)(uint8_t* data);

// Move-only ownership for one encoded screenshot. Platform allocates and
// releases the bytes; Host may detach them when handing the artifact to a
// bounded transport queue.
class ScreenCapture final {
   public:
    ScreenCapture() = default;
    ScreenCapture(uint8_t* data, size_t size, uint32_t width, uint32_t height, ScreenCaptureRelease release)
        : data_(data), size_(size), width_(width), height_(height), release_(release) {}
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;
    ScreenCapture(ScreenCapture&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0U)),
          width_(std::exchange(other.width_, 0U)),
          height_(std::exchange(other.height_, 0U)),
          release_(std::exchange(other.release_, nullptr)) {}
    ScreenCapture& operator=(ScreenCapture&& other) noexcept {
        if (this != &other) {
            Reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0U);
            width_ = std::exchange(other.width_, 0U);
            height_ = std::exchange(other.height_, 0U);
            release_ = std::exchange(other.release_, nullptr);
        }
        return *this;
    }
    ~ScreenCapture() { Reset(); }

    [[nodiscard]] bool valid() const { return data_ != nullptr && size_ != 0U && release_ != nullptr; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }
    [[nodiscard]] ScreenCaptureRelease releaser() const { return release_; }
    [[nodiscard]] uint8_t* Detach() {
        size_ = 0U;
        width_ = 0U;
        height_ = 0U;
        release_ = nullptr;
        return std::exchange(data_, nullptr);
    }

   private:
    void Reset() {
        if (data_ != nullptr && release_ != nullptr) {
            release_(data_);
        }
        data_ = nullptr;
        size_ = 0U;
        width_ = 0U;
        height_ = 0U;
        release_ = nullptr;
    }

    uint8_t* data_{};
    size_t size_{};
    uint32_t width_{};
    uint32_t height_{};
    ScreenCaptureRelease release_{};
};

struct HallAppModel final {
    const char* app_id{};
    const char* display_name{};
    HallCoverModel cover{};
    uint8_t install_progress_percent{};
    bool running{};
    bool installing{};
};

struct HallWifiModel final {
    std::array<char, kMaxWifiSsidLength + 1U> ssid{};
    int8_t rssi{};
    bool available{};
    bool enabled{};
    bool connected{};
};

struct HallBatteryModel final {
    uint8_t percent{};
    bool available{};
    bool charging{};
};

struct HallCellularModel final {
    uint8_t signal_bars{};
    bool available{};
    bool enabled{};
    bool connected{};
};

struct HallStatusBarModel final {
    std::array<char, 6U> time_text{};
    HallWifiModel wifi{};
    HallCellularModel cellular{};
    HallBatteryModel battery{};
};

struct HallModel final {
    std::array<HallAppModel, kMaxHallApps> apps{};
    uint32_t app_count{};
    const char* status_app_id{};
    const char* status_error_phase{};
    const char* status_error_code{};
    const char* status_error_detail{};
    HallStatus status{HallStatus::kReady};
    uint32_t detail{};
    int32_t status_exit_code{};
    bool status_has_exit_code{};
    bool launch_enabled{};
    HallStatusBarModel status_bar{};
    uint64_t transition_trigger_us{};
    bool firmware_update_available{};
    bool install_active{};
};

enum class SystemThemeMode : uint8_t {
    kPureBlack,
    kDeepBlue,
    kSoftIvory,
};

struct StatusLayerModel final {
    uint32_t memory_used_kib{};
    uint32_t memory_total_kib{};
    uint32_t sram_used_kib{};
    uint32_t sram_total_kib{};
    uint32_t storage_used_kib{};
    uint32_t storage_total_kib{};
    uint8_t battery_percent{};
    uint8_t brightness_percent{80U};
    uint8_t volume_percent{70U};
    uint8_t auto_sleep_timeout_minutes{kDefaultAutoSleepTimeoutMinutes};
    SystemThemeMode theme_mode{SystemThemeMode::kPureBlack};
    bool wifi_available{};
    bool wifi_enabled{};
    bool wifi_connected{};
    bool wifi_connecting{};
    bool cellular_available{};
    bool cellular_enabled{};
    bool cellular_connected{};
    bool battery_available{};
    bool battery_charging{};
    bool battery_discharging{};
    bool battery_charging_available{};
    bool external_power_connected{};
    bool external_power_available{};
    bool performance_overlay_enabled{};
};

enum class SystemMenuItem : uint32_t {
    kWifi,
    kRemoteControl,
    kPowerManagement,
    kAppearance,
    kLanguage,
    kSystemInformation,
    kManageApps,
};

struct SystemMenuModel final {
    const char* locale{"en"};
    const char* language{"English"};
    uint32_t installed_app_count{};
    uint8_t auto_sleep_timeout_minutes{kDefaultAutoSleepTimeoutMinutes};
    SystemThemeMode theme_mode{SystemThemeMode::kPureBlack};
    bool wifi_available{};
    bool wifi_enabled{};
    bool wifi_connected{};
    bool wifi_connecting{};
    bool remote_control_enabled{};
    bool remote_control_connected{};
    bool firmware_update_available{};
    std::array<char, kFirmwareVersionTextCapacity> latest_firmware_version{};
    std::array<char, kFirmwareUpdateMessageCapacity> firmware_update_message{};
    FirmwareUpdateState firmware_update_state{FirmwareUpdateState::kUnknown};
};

struct PowerManagementModel final {
    uint8_t auto_sleep_timeout_minutes{kDefaultAutoSleepTimeoutMinutes};
};

struct AppearanceModel final {
    SystemThemeMode theme_mode{SystemThemeMode::kPureBlack};
};

enum class RemoteControlConnectionState : uint8_t {
    kDisabled,
    kWaitingForNetwork,
    kConnecting,
    kConnected,
    kBackoff,
    kAuthenticationError,
};

constexpr uint32_t kRemoteControlServiceTextCapacity = 64U;
constexpr uint32_t kRemoteControlDeviceIdCapacity = 37U;
constexpr uint32_t kRemoteControlPairingCodeCapacity = 10U;
constexpr uint32_t kRemoteControlStatusTextCapacity = 96U;
struct RemoteControlModel final {
    std::array<char, kRemoteControlServiceTextCapacity> service{};
    std::array<char, kRemoteControlDeviceIdCapacity> device_id{};
    std::array<char, kRemoteControlPairingCodeCapacity> pairing_code{};
    std::array<char, kRemoteControlStatusTextCapacity> status_message{};
    std::array<char, kFirmwareVersionTextCapacity> latest_firmware_version{};
    std::array<char, kRemoteControlStatusTextCapacity> firmware_update_message{};
    uint32_t pairing_expires_seconds{};
    uint32_t firmware_size_bytes{};
    uint32_t firmware_processed_bytes{};
    uint8_t firmware_progress_percent{};
    RemoteControlConnectionState connection_state{RemoteControlConnectionState::kDisabled};
    FirmwareUpdateState firmware_update_state{FirmwareUpdateState::kUnknown};
    bool enabled{};
    bool pairing_code_pending{};
    bool pairing_code_available{};
    bool firmware_update_available{};
    bool firmware_update_installable{};
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
    std::array<char, kSystemInformationTextCapacity> latest_firmware_version{};
    std::array<char, kRemoteControlStatusTextCapacity> firmware_update_message{};
    std::array<char, kSystemInformationTextCapacity> build_date{};
    std::array<char, kSystemInformationTextCapacity> build_time{};
    std::array<char, kSystemInformationTextCapacity> build_id{};
    std::array<char, kSystemInformationTextCapacity> idf_version{};
    std::array<char, kSystemInformationTextCapacity> host_chip{};
    std::array<char, kSystemInformationTextCapacity> cpu{};
    std::array<char, kSystemInformationTextCapacity> wifi_coprocessor{};
    std::array<char, kSystemInformationTextCapacity> wifi_mac{};
    std::array<char, kSystemInformationTextCapacity> flash_capacity{};
    std::array<char, kSystemInformationTextCapacity> panel{};
    std::array<char, kSystemInformationTextCapacity> display_interface{};
    std::array<char, kSystemInformationTextCapacity> resolution{};
    std::array<char, kSystemInformationTextCapacity> touch_controller{};
    std::array<char, kSystemInformationTextCapacity> graphics_acceleration{};
    std::array<char, kSystemInformationTextCapacity> uptime{};
    std::array<char, kSystemInformationTextCapacity> last_reset{};
    MemoryStatisticsModel internal_sram{};
    MemoryStatisticsModel psram{};
    uint32_t firmware_size_bytes{};
    uint32_t firmware_processed_bytes{};
    uint8_t firmware_progress_percent{};
    FirmwareUpdateState firmware_update_state{FirmwareUpdateState::kUnknown};
    bool firmware_update_available{};
    bool firmware_update_installable{};
    bool firmware_update_view{};
};

struct InstalledAppModel final {
    const char* app_id{};
    const char* display_name{};
    uint32_t bundle_size_kib{};
};

struct AppManagementModel final {
    std::array<InstalledAppModel, kMaxHallApps> apps{};
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
    kOpenFirmwareUpdate,
    kCloseSystemMenu,
    kSelectSystemMenuItem,
    kCloseSystemInformation,
    kInstallFirmwareUpdate,
    kClosePowerManagement,
    kSetAutoSleepEnabled,
    kSetAutoSleepTimeout,
    kCloseAppearance,
    kSetThemeMode,
    kCloseRemoteControl,
    kSetRemoteControlEnabled,
    kGenerateRemoteControlPairingCode,
    kCancelRemoteControlPairingCode,
    kCloseAppManagement,
    kLaunchInstalledApp,
    kUninstallInstalledApp,
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
    // Internal Host wake-up signals. SystemUi implementations never emit
    // these actions directly.
    kPowerButtonPressed,
    kPowerOffRequested,
    kWifiStateChanged,
    kBatteryStateChanged,
    kTimeStateChanged,
    kRemoteCommandReady,
    kUserActivity,
};

struct SystemUiAction final {
    SystemUiActionType type{SystemUiActionType::kLaunchApp};
    uint32_t app_index{};
    uint32_t value{};
    uint64_t timestamp_us{};
    std::array<char, kMaxWifiSsidLength + 1U> text{};
    std::array<char, kMaxWifiPasswordLength + 1U> secret{};
};

using SystemUiActionSink = void (*)(void* context, const SystemUiAction& action);

enum class SystemUiError {
    kUnavailable,
    kRenderFailed,
};

class SystemUi {
   public:
    virtual ~SystemUi() = default;
    SystemUi(const SystemUi&) = delete;
    SystemUi& operator=(const SystemUi&) = delete;

    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowHall(const HallModel& model,
                                                                      SystemUiActionSink action_sink,
                                                                      void* action_context) = 0;
    virtual void UpdateHallStatusBar(const HallStatusBarModel& model) = 0;
    virtual void UpdateHallInstallProgress(uint32_t app_index, uint8_t progress_percent) {
        (void)app_index;
        (void)progress_percent;
    }
    virtual void PauseHallCoverLoading() {}
    virtual void PrepareAppLaunch(uint32_t app_index) { (void)app_index; }
    virtual void LeaveHall() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> RestoreGuestView() = 0;
    virtual void WatchGuestActions(SystemUiActionSink action_sink, void* action_context) = 0;
    virtual void StopWatchingGuestActions(void* action_context) = 0;
    [[nodiscard]] virtual std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame(
        uint32_t hall_app_index, uint64_t trigger_timestamp_us) = 0;
    [[nodiscard]] virtual std::expected<ScreenCapture, SystemUiError> CaptureScreenJpeg() {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    [[nodiscard]] virtual bool SupportsScreenCapture() const { return false; }
    virtual void ReleaseGuestSnapshot() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowSystemMenu(const SystemMenuModel& model,
                                                                            SystemUiActionSink action_sink,
                                                                            void* action_context) = 0;
    virtual void UpdateSystemMenu(const SystemMenuModel& model) = 0;
    virtual void LeaveSystemMenu() = 0;
    virtual void ApplyTheme(SystemThemeMode mode) { (void)mode; }
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowSystemInformation(const SystemInformationModel& model,
                                                                                   SystemUiActionSink action_sink,
                                                                                   void* action_context) = 0;
    virtual void UpdateSystemInformation(const SystemInformationModel&) {}
    virtual void LeaveSystemInformation() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowPowerManagement(const PowerManagementModel& model,
                                                                                 SystemUiActionSink action_sink,
                                                                                 void* action_context) = 0;
    virtual void UpdatePowerManagement(const PowerManagementModel& model) = 0;
    virtual void LeavePowerManagement() = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowAppearance(const AppearanceModel& model,
                                                                            SystemUiActionSink action_sink,
                                                                            void* action_context) {
        (void)model;
        (void)action_sink;
        (void)action_context;
        return std::unexpected(SystemUiError::kUnavailable);
    }
    virtual void UpdateAppearance(const AppearanceModel& model) { (void)model; }
    virtual void LeaveAppearance() {}
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowRemoteControl(const RemoteControlModel& model,
                                                                               SystemUiActionSink action_sink,
                                                                               void* action_context) = 0;
    virtual void UpdateRemoteControl(const RemoteControlModel& model) = 0;
    virtual void LeaveRemoteControl() = 0;
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
                                                                             uint64_t trigger_timestamp_us,
                                                                             SystemUiActionSink action_sink,
                                                                             void* action_context) = 0;
    virtual void UpdateStatusLayer(const StatusLayerModel& model) = 0;
    virtual void LeaveStatusLayer(uint64_t trigger_timestamp_us) = 0;
    virtual void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) = 0;
    virtual void ApplyBrightness(uint8_t percent) = 0;
    virtual void ApplyVolume(uint8_t percent) = 0;
    [[nodiscard]] virtual std::expected<void, SystemUiError> ShowShutdown() = 0;

   protected:
    SystemUi() = default;
};

}  // namespace micropixel::host_ui

#endif
